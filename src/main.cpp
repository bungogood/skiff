#include "skiff/benchmark_protocol.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <unordered_map>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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

[[nodiscard]] int parse_port(std::string_view value) {
  char* end{};
  const auto port = std::strtol(value.data(), &end, 10);
  if (*end != '\0' || port < 1 || port > 65535) {
    return 0;
  }
  return static_cast<int>(port);
}

}  // namespace

int main(int argc, char* argv[]) {
  constexpr int default_port = 9000;
  int port = default_port;
  if (argc == 3 && std::string_view{argv[1]} == "--port") {
    port = parse_port(argv[2]);
  } else if (argc != 1) {
    std::cerr << "usage: skiff_node [--port PORT]\n";
    return 2;
  }
  if (port == 0) {
    std::cerr << "invalid port\n";
    return 2;
  }

  std::signal(SIGPIPE, SIG_IGN);
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    std::perror("socket");
    return 1;
  }

  int reuse_address = 1;
  if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
    std::perror("setsockopt");
    close(listener);
    return 1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    std::perror("bind");
    close(listener);
    return 1;
  }
  if (listen(listener, SOMAXCONN) < 0) {
    std::perror("listen");
    close(listener);
    return 1;
  }

  std::unordered_map<std::uint64_t, std::uint64_t> values;
  std::cout << "skiff_node in-memory primary listening on port " << port << '\n';
  for (;;) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("accept");
      close(listener);
      return 1;
    }
    std::array<std::byte, skiff::benchmark::request_size * max_batch_frames +
                              skiff::benchmark::request_size - 1>
        request_bytes{};
    std::array<std::byte, skiff::benchmark::response_size * max_batch_frames> response_bytes{};
    std::size_t buffered{};
    for (;;) {
      const auto received = recv(client, request_bytes.data() + buffered,
                                 request_bytes.size() - buffered, 0);
      if (received <= 0) {
        break;
      }
      buffered += static_cast<std::size_t>(received);
      const auto request_count = buffered / skiff::benchmark::request_size;
      for (std::size_t index = 0; index < request_count; ++index) {
        const auto offset = index * skiff::benchmark::request_size;
        const auto request = skiff::benchmark::decode_request(
            std::span<const std::byte, skiff::benchmark::request_size>{request_bytes.data() + offset,
                                                                         skiff::benchmark::request_size});
        values[request.key] = request.value;
        skiff::benchmark::encode_response(
            request.id, response_bytes.data() + index * skiff::benchmark::response_size);
      }
      if (!transfer_all(
              client,
              std::span{response_bytes}.first(request_count * skiff::benchmark::response_size), false)) {
        break;
      }

      const auto consumed = request_count * skiff::benchmark::request_size;
      buffered -= consumed;
      std::memmove(request_bytes.data(), request_bytes.data() + consumed, buffered);
    }
    close(client);
  }
}
