#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace skiff::benchmark {

inline constexpr std::size_t request_size = 24;
inline constexpr std::size_t response_size = 8;

struct Request {
  std::uint64_t id;
  std::uint64_t key;
  std::uint64_t value;
};

inline void encode_u64(std::uint64_t value, std::byte* destination) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[sizeof(value) - 1 - index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

[[nodiscard]] inline std::uint64_t decode_u64(const std::byte* source) {
  std::uint64_t value{};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value = (value << 8U) | std::to_integer<std::uint8_t>(source[index]);
  }
  return value;
}

inline void encode_request(Request request, std::byte* destination) {
  encode_u64(request.id, destination);
  encode_u64(request.key, destination + sizeof(request.id));
  encode_u64(request.value, destination + sizeof(request.id) + sizeof(request.key));
}

[[nodiscard]] inline std::array<std::byte, request_size> encode_request(Request request) {
  std::array<std::byte, request_size> bytes{};
  encode_request(request, bytes.data());
  return bytes;
}

[[nodiscard]] inline Request decode_request(std::span<const std::byte, request_size> bytes) {
  return {
      .id = decode_u64(bytes.data()),
      .key = decode_u64(bytes.data() + sizeof(std::uint64_t)),
      .value = decode_u64(bytes.data() + 2 * sizeof(std::uint64_t)),
  };
}

inline void encode_response(std::uint64_t id, std::byte* destination) {
  encode_u64(id, destination);
}

[[nodiscard]] inline std::array<std::byte, response_size> encode_response(std::uint64_t id) {
  std::array<std::byte, response_size> bytes{};
  encode_response(id, bytes.data());
  return bytes;
}

[[nodiscard]] inline std::uint64_t decode_response(
    std::span<const std::byte, response_size> bytes) {
  return decode_u64(bytes.data());
}

}  // namespace skiff::benchmark
