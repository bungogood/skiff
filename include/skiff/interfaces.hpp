#pragma once

#include "skiff/types.hpp"

#include <span>

namespace skiff {

class Clock {
 public:
  virtual ~Clock() = default;
  virtual MonotonicTime now() const = 0;
};

class Transport {
 public:
  virtual ~Transport() = default;
  virtual void send(Message message) = 0;
};

class Storage {
 public:
  virtual ~Storage() = default;
  virtual void persist_term_and_vote(Term term, NodeId voted_for) = 0;
  virtual void append(LogIndex index, Term term, std::span<const std::byte> command) = 0;
  virtual void sync() = 0;
};

class StateMachine {
 public:
  virtual ~StateMachine() = default;
  virtual void apply(LogIndex index, std::span<const std::byte> command) = 0;
};

}  // namespace skiff
