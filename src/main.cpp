#include "skiff/benchmark_protocol.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <unordered_map>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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

    std::array<std::byte, skiff::benchmark::request_size> request_bytes{};
    while (transfer_all(client, request_bytes, true)) {
      const auto request = skiff::benchmark::decode_request(request_bytes);
      values[request.key] = request.value;
      auto response = skiff::benchmark::encode_response(request.id);
      if (!transfer_all(client, response, false)) {
        break;
      }
    }
    close(client);
  }
}
