#pragma once

#include "skiff/interfaces.hpp"

#include <optional>
#include <span>
#include <vector>

namespace skiff {

enum class ReplicationMode {
  // One primary only. No network replication.
  single_node,
  // A fixed primary replicates best-effort after locally acknowledging a write.
  async_primary_replication,
  // Raft election and quorum commit. Not implemented yet.
  raft,
};

struct RaftConfig {
  NodeId node_id;
  NodeId primary_id;
  std::vector<NodeId> peer_ids;
  ReplicationMode replication_mode{ReplicationMode::raft};
};

class RaftNode {
 public:
  RaftNode(RaftConfig config, Clock& clock, Transport& transport, Storage& storage,
           StateMachine& state_machine);

  void tick();
  void receive(Message message);

  // Returns no index when this node cannot accept a write in the selected mode.
  [[nodiscard]] std::optional<LogIndex> submit(std::span<const std::byte> command);

  [[nodiscard]] Role role() const;
  [[nodiscard]] Term current_term() const;

 private:
  RaftConfig config_;
  Clock& clock_;
  [[maybe_unused]] Transport& transport_;
  [[maybe_unused]] Storage& storage_;
  [[maybe_unused]] StateMachine& state_machine_;
  Role role_{Role::follower};
  Term current_term_{0};
  LogIndex last_log_index_{0};
};

}  // namespace skiff
