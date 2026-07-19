#include "skiff/raft_node.hpp"

#include <utility>

namespace skiff {

RaftNode::RaftNode(RaftConfig config, Clock& clock, Transport& transport, Storage& storage,
                   StateMachine& state_machine)
    : config_(config),
      clock_(clock),
       transport_(transport),
       storage_(storage),
       state_machine_(state_machine) {
  if (config_.replication_mode != ReplicationMode::raft && config_.node_id == config_.primary_id) {
    role_ = Role::primary;
  }
}

void RaftNode::tick() {
  static_cast<void>(clock_);
}

void RaftNode::receive(Message message) {
  if (config_.replication_mode != ReplicationMode::async_primary_replication ||
      config_.node_id == config_.primary_id || message.kind != MessageKind::append_entries ||
      message.sender != config_.primary_id || message.recipient != config_.node_id ||
      message.log_index != last_log_index_ + 1) {
    return;
  }

  last_log_index_ = message.log_index;
  storage_.append(last_log_index_, message.term, message.payload);
  storage_.sync();
  state_machine_.apply(last_log_index_, message.payload);
}

std::optional<LogIndex> RaftNode::submit(std::span<const std::byte> command) {
  if (config_.replication_mode == ReplicationMode::raft || role_ != Role::primary) {
    return std::nullopt;
  }

  const auto index = ++last_log_index_;
  storage_.append(index, current_term_, command);
  storage_.sync();
  state_machine_.apply(index, command);

  if (config_.replication_mode == ReplicationMode::async_primary_replication) {
    for (const auto peer_id : config_.peer_ids) {
      transport_.send({
          .kind = MessageKind::append_entries,
          .sender = config_.node_id,
          .recipient = peer_id,
          .term = current_term_,
          .log_index = index,
          .payload = {command.begin(), command.end()},
      });
    }
  }

  return index;
}

Role RaftNode::role() const {
  return role_;
}

Term RaftNode::current_term() const {
  return current_term_;
}

}  // namespace skiff
