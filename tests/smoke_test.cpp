#include "skiff/raft_node.hpp"
#include "skiff/benchmark_protocol.hpp"
#include "fakes.hpp"

#include <cassert>
#include <array>

int main() {
  const skiff::benchmark::Request wire_request{.id = 7, .key = 8, .value = 9};
  assert(skiff::benchmark::decode_request(skiff::benchmark::encode_request(wire_request)).id == 7);
  assert(skiff::benchmark::decode_request(skiff::benchmark::encode_request(wire_request)).key == 8);
  assert(skiff::benchmark::decode_request(skiff::benchmark::encode_request(wire_request)).value == 9);
  assert(skiff::benchmark::decode_response(skiff::benchmark::encode_response(10)) == 10);

  skiff::test::FakeClock clock;
  skiff::test::CapturingTransport transport;
  skiff::test::MemoryStorage storage;
  skiff::test::RecordingStateMachine state_machine;
  skiff::RaftNode node({.node_id = 1, .primary_id = 1}, clock, transport, storage, state_machine);

  const skiff::Message message{
      .kind = skiff::MessageKind::append_entries,
      .sender = 1,
      .recipient = 2,
      .term = 3,
      .log_index = 1,
      .payload = {},
  };

  assert(message.sender == 1);
  assert(message.recipient == 2);
  assert(message.term == 3);
  assert(node.role() == skiff::Role::follower);
  assert(node.current_term() == 0);

  clock.advance(std::chrono::milliseconds{100});
  node.tick();

  const std::array command{std::byte{0x42}};
  assert(!node.submit(command).has_value());

  skiff::RaftNode single_node(
      {.node_id = 1, .primary_id = 1, .replication_mode = skiff::ReplicationMode::single_node},
      clock, transport, storage, state_machine);
  assert(single_node.role() == skiff::Role::primary);
  assert(single_node.current_term() == 0);
  assert(single_node.submit(command) == 1);
  assert(storage.entries.contains(1));
  assert(storage.sync_count == 1);
  assert(state_machine.applied_indexes == std::vector<skiff::LogIndex>{1});

  skiff::test::MemoryStorage replica_storage;
  skiff::test::RecordingStateMachine replica_state_machine;
  skiff::RaftNode primary(
      {.node_id = 1,
       .primary_id = 1,
       .peer_ids = {2, 3},
       .replication_mode = skiff::ReplicationMode::async_primary_replication},
      clock, transport, storage, state_machine);
  skiff::RaftNode replica(
      {.node_id = 2,
       .primary_id = 1,
       .peer_ids = {1, 3},
       .replication_mode = skiff::ReplicationMode::async_primary_replication},
      clock, transport, replica_storage, replica_state_machine);
  assert(primary.submit(command) == 1);
  assert(transport.messages.size() == 2);
  replica.receive(transport.messages.front());
  assert(replica_storage.entries.contains(1));
  assert(replica_state_machine.applied_indexes == std::vector<skiff::LogIndex>{1});
}
