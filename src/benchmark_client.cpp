#include "skiff/benchmark_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t max_pipeline = 65'536;
constexpr std::size_t max_batch_frames = 256;

bool transfer_all(int socket_fd, std::span<std::byte> buffer, bool receive) {
  std::size_t transferred{};
  while (transferred < buffer.size()) {
    const auto result = receive
                            ? recv(socket_fd, buffer.data() + transferred, buffer.size() - transferred, 0)
                            : send(socket_fd, buffer.data() + transferred, buffer.size() - transferred, 0);
    if (result <= 0) {
      return false;
    }
    transferred += static_cast<std::size_t>(result);
  }
  return true;
}

bool configure_busy_poll(int socket_fd, unsigned int duration_us) {
  if (duration_us == 0) {
    return true;
  }
#ifdef SO_BUSY_POLL
  return setsockopt(socket_fd, SOL_SOCKET, SO_BUSY_POLL, &duration_us, sizeof(duration_us)) == 0;
#else
  static_cast<void>(socket_fd);
  errno = ENOTSUP;
  return false;
#endif
}

template <typename T>
bool parse_unsigned(std::string_view value, T& output) {
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), output);
  return error == std::errc{} && end == value.data() + value.size() && output > 0;
}

[[nodiscard]] int connect_to(std::string_view host, std::string_view port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses{};
  if (getaddrinfo(host.data(), port.data(), &hints, &addresses) != 0) {
    return -1;
  }

  int socket_fd = -1;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd >= 0 && connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    if (socket_fd >= 0) {
      close(socket_fd);
      socket_fd = -1;
    }
  }
  freeaddrinfo(addresses);
  return socket_fd;
}

[[nodiscard]] double percentile(const std::vector<double>& values, double percent) {
  const auto index = static_cast<std::size_t>(percent * static_cast<double>(values.size() - 1));
  return values[index];
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::cerr << "usage: skiff_bench HOST PORT REQUESTS [PIPELINE] [--busy-poll-us USEC]\n";
    return 2;
  }

  std::uint64_t request_count{};
  std::uint64_t pipeline{64};
  unsigned int busy_poll_us{};
  int option_index = 4;
  if (option_index < argc && std::string_view{argv[option_index]} != "--busy-poll-us") {
    if (!parse_unsigned(argv[option_index], pipeline)) {
      std::cerr << "REQUESTS and PIPELINE must be positive integers; PIPELINE is at most "
                << max_pipeline << '\n';
      return 2;
    }
    ++option_index;
  }
  if (!parse_unsigned(argv[3], request_count) || pipeline > max_pipeline ||
      (option_index < argc && (option_index + 2 != argc ||
                               std::string_view{argv[option_index]} != "--busy-poll-us" ||
                               !parse_unsigned(argv[option_index + 1], busy_poll_us)))) {
    std::cerr << "REQUESTS and PIPELINE must be positive integers; PIPELINE is at most "
              << max_pipeline << '\n';
    return 2;
  }

  std::signal(SIGPIPE, SIG_IGN);
  const int socket_fd = connect_to(argv[1], argv[2]);
  if (socket_fd < 0) {
    std::cerr << "unable to connect to " << argv[1] << ':' << argv[2] << '\n';
    return 1;
  }
  if (!configure_busy_poll(socket_fd, busy_poll_us)) {
    std::perror("setsockopt SO_BUSY_POLL");
    close(socket_fd);
    return 1;
  }

  std::vector<Clock::time_point> sent_at(request_count);
  std::vector<double> latencies_us;
  latencies_us.reserve(request_count);
  std::array<std::byte, skiff::benchmark::request_size * max_batch_frames> request_bytes{};
  std::array<std::byte, skiff::benchmark::response_size * max_batch_frames +
                            skiff::benchmark::response_size - 1>
      response_bytes{};
  std::uint64_t sent{};
  std::uint64_t completed{};
  std::size_t buffered_responses{};
  const auto start = Clock::now();
  while (completed < request_count) {
    const auto batch_count = std::min<std::uint64_t>(
        {request_count - sent, pipeline - (sent - completed), max_batch_frames});
    for (std::uint64_t index = 0; index < batch_count; ++index) {
      const auto request_id = sent + index;
      skiff::benchmark::encode_request(
          {.id = request_id, .key = request_id % 1'000'000U, .value = request_id},
          request_bytes.data() + index * skiff::benchmark::request_size);
      sent_at[request_id] = Clock::now();
    }
    if (batch_count > 0 &&
        !transfer_all(socket_fd,
                      std::span{request_bytes}.first(batch_count * skiff::benchmark::request_size), false)) {
        std::cerr << "write failed\n";
        close(socket_fd);
        return 1;
    }
    sent += batch_count;

    const auto received = recv(socket_fd, response_bytes.data() + buffered_responses,
                               response_bytes.size() - buffered_responses, 0);
    if (received <= 0) {
      std::cerr << "read failed\n";
      close(socket_fd);
      return 1;
    }
    buffered_responses += static_cast<std::size_t>(received);
    const auto response_count = buffered_responses / skiff::benchmark::response_size;
    for (std::size_t index = 0; index < response_count; ++index) {
      const auto offset = index * skiff::benchmark::response_size;
      const auto response_id = skiff::benchmark::decode_response(
          std::span<const std::byte, skiff::benchmark::response_size>{response_bytes.data() + offset,
                                                                       skiff::benchmark::response_size});
      if (response_id != completed) {
        std::cerr << "response order mismatch\n";
        close(socket_fd);
        return 1;
      }
      const auto elapsed = Clock::now() - sent_at[response_id];
      latencies_us.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
      ++completed;
    }
    const auto consumed = response_count * skiff::benchmark::response_size;
    buffered_responses -= consumed;
    std::memmove(response_bytes.data(), response_bytes.data() + consumed, buffered_responses);
  }
  const auto duration = std::chrono::duration<double>(Clock::now() - start).count();
  close(socket_fd);
  std::sort(latencies_us.begin(), latencies_us.end());

  std::cout << "mode=single_node_in_memory\n";
  std::cout << "requests=" << request_count << '\n';
  std::cout << "pipeline=" << pipeline << '\n';
  std::cout << "busy_poll_us=" << busy_poll_us << '\n';
  std::cout << "duration_seconds=" << duration << '\n';
  std::cout << "operations_per_second=" << static_cast<double>(request_count) / duration << '\n';
  std::cout << "latency_us_p50=" << percentile(latencies_us, 0.50) << '\n';
  std::cout << "latency_us_p95=" << percentile(latencies_us, 0.95) << '\n';
  std::cout << "latency_us_p99=" << percentile(latencies_us, 0.99) << '\n';
  std::cout << "latency_us_p999=" << percentile(latencies_us, 0.999) << '\n';
  std::cout << "latency_us_max=" << latencies_us.back() << '\n';
}
