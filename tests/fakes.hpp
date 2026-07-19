#pragma once

#include "skiff/interfaces.hpp"

#include <map>
#include <utility>
#include <vector>

namespace skiff::test {

class FakeClock final : public Clock {
 public:
  [[nodiscard]] MonotonicTime now() const override {
    return now_;
  }

  void advance(std::chrono::milliseconds duration) {
    now_ += duration;
  }

 private:
  MonotonicTime now_{};
};

class CapturingTransport final : public Transport {
 public:
  void send(Message message) override {
    messages.push_back(std::move(message));
  }

  std::vector<Message> messages;
};

class MemoryStorage final : public Storage {
 public:
  void persist_term_and_vote(Term term, NodeId voted_for) override {
    persisted_term = term;
    persisted_vote = voted_for;
  }

  void append(LogIndex index, Term term, std::span<const std::byte> command) override {
    entries[index] = Entry{.term = term, .command = {command.begin(), command.end()}};
  }

  void sync() override {
    ++sync_count;
  }

  struct Entry {
    Term term;
    std::vector<std::byte> command;
  };

  Term persisted_term{};
  NodeId persisted_vote{};
  std::map<LogIndex, Entry> entries;
  std::size_t sync_count{};
};

class RecordingStateMachine final : public StateMachine {
 public:
  void apply(LogIndex index, std::span<const std::byte> command) override {
    applied_indexes.push_back(index);
    applied_commands.emplace_back(command.begin(), command.end());
  }

  std::vector<LogIndex> applied_indexes;
  std::vector<std::vector<std::byte>> applied_commands;
};

}  // namespace skiff::test
