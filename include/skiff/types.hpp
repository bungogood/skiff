#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace skiff {

using NodeId = std::uint32_t;
using Term = std::uint64_t;
using LogIndex = std::uint64_t;
using MonotonicTime = std::chrono::steady_clock::time_point;

enum class Role {
  follower,
  primary,
  candidate,
  leader,
};

enum class MessageKind {
  request_vote,
  request_vote_response,
  append_entries,
  append_entries_response,
};

struct Message {
  MessageKind kind;
  NodeId sender;
  NodeId recipient;
  Term term;
  LogIndex log_index;
  std::vector<std::byte> payload;
};

}  // namespace skiff
