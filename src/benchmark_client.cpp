#include "skiff/benchmark_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
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

[[nodiscard]] double percentile(std::vector<double> values, double percent) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(percent * static_cast<double>(values.size() - 1));
  return values[index];
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4 && argc != 5) {
    std::cerr << "usage: skiff_bench HOST PORT REQUESTS [PIPELINE]\n";
    return 2;
  }

  std::uint64_t request_count{};
  std::uint64_t pipeline{64};
  if (!parse_unsigned(argv[3], request_count) ||
      (argc == 5 && !parse_unsigned(argv[4], pipeline))) {
    std::cerr << "REQUESTS and PIPELINE must be positive integers\n";
    return 2;
  }

  std::signal(SIGPIPE, SIG_IGN);
  const int socket_fd = connect_to(argv[1], argv[2]);
  if (socket_fd < 0) {
    std::cerr << "unable to connect to " << argv[1] << ':' << argv[2] << '\n';
    return 1;
  }

  std::vector<Clock::time_point> sent_at(request_count);
  std::vector<double> latencies_us;
  latencies_us.reserve(request_count);
  std::uint64_t sent{};
  std::uint64_t completed{};
  const auto start = Clock::now();
  while (completed < request_count) {
    while (sent < request_count && sent - completed < pipeline) {
      auto request = skiff::benchmark::encode_request({
          .id = sent,
          .key = sent % 1'000'000U,
          .value = sent,
      });
      sent_at[sent] = Clock::now();
      if (!transfer_all(socket_fd, request, false)) {
        std::cerr << "write failed\n";
        close(socket_fd);
        return 1;
      }
      ++sent;
    }

    std::array<std::byte, skiff::benchmark::response_size> response{};
    if (!transfer_all(socket_fd, response, true)) {
      std::cerr << "read failed\n";
      close(socket_fd);
      return 1;
    }
    const auto response_id = skiff::benchmark::decode_response(response);
    if (response_id != completed) {
      std::cerr << "response order mismatch\n";
      close(socket_fd);
      return 1;
    }
    const auto elapsed = Clock::now() - sent_at[response_id];
    latencies_us.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
    ++completed;
  }
  const auto duration = std::chrono::duration<double>(Clock::now() - start).count();
  close(socket_fd);

  std::cout << "mode=single_node_in_memory\n";
  std::cout << "requests=" << request_count << '\n';
  std::cout << "pipeline=" << pipeline << '\n';
  std::cout << "duration_seconds=" << duration << '\n';
  std::cout << "operations_per_second=" << static_cast<double>(request_count) / duration << '\n';
  std::cout << "latency_us_p50=" << percentile(latencies_us, 0.50) << '\n';
  std::cout << "latency_us_p95=" << percentile(latencies_us, 0.95) << '\n';
  std::cout << "latency_us_p99=" << percentile(latencies_us, 0.99) << '\n';
}
