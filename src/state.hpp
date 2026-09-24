// Optical Fabric 1.0.0 - Summon Software Labs
// Internal state records.
//
// These are the authoritative in-memory records. They are also exactly what the
// store persists, and they are applied through a single code path
// (decode -> apply) so that live mutation and recovery can never diverge.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/fabric.hpp"
#include "text_codec.hpp"

namespace optical_fabric::detail {

struct ResourceState {
  ResourceRef resource{};
  std::string name;
  SharingMode sharing = SharingMode::Exclusive;
  Generation generation{};
  Tick registered_tick{};
  std::vector<ResourceRef> related;
  std::vector<std::string> attributes;

  // Structural links, validated at registration time and used instead of
  // re-parsing the attribute list.
  SiteId site{};
  OpticalNodeId node{};
  PortId endpoint_a{};
  PortId endpoint_b{};
  PortId ingress{};
  PortId egress{};
  std::vector<SpanId> spans;
  std::vector<PortId> endpoints;

  std::string role;
  std::string region;
  std::string description;
  std::string band;
  std::uint32_t channel_index = 0;
  std::uint64_t nominal_frequency_ghz = 0;
  std::uint64_t declared_length_metres = 0;
};

struct ClaimEntry {
  ResourceRef resource{};
  ConnectivityId connectivity{};
  ChannelId channel{};
  bool exclusive = true;
  Tick committed_tick{};
};

struct ConnectivityRecord {
  ConnectivityId id{};
  std::string name;
  std::string owner;
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  PathDescriptor path;
  CanonicalPath canonical;
  IntentRequirements requirements;
  ReservationId reservation{};
  bool has_reservation = false;
  Tick reservation_expiry{};
  AuthorityToken authority{};
  bool authority_confirmed = false;
  Tick created_tick{};
  Tick updated_tick{};
  Tick activated_tick{};
  Tick retired_tick{};
  RefusalCode last_refusal = RefusalCode::None;
  std::string last_refusal_detail;
  Digest128 activation_digest{};
  AttemptId activation_attempt{};
  std::vector<TransitionRecord> history;
  std::size_t transitions_dropped = 0;
  std::vector<AttemptId> attempt_log;
};

struct ReservationState {
  ReservationView view;
};

struct GrantState {
  AuthorityGrantView view;
  FenceRecord fence;
  bool has_fence = false;
};

struct AttemptRecord {
  AttemptId attempt{};
  Digest128 request_digest{};
  std::string operation;
  OutcomeCode outcome = OutcomeCode::Applied;
  RefusalCode refusal = RefusalCode::None;
  ConnectivityId connectivity{};
  ReservationId reservation{};
  GrantId grant{};
  Generation generation{};
  Tick tick{};
  std::string summary;
};

struct SourceRegistration {
  std::shared_ptr<IEvidenceSource> source;
  SourceDescriptor descriptor;
};

struct RuntimeState {
  Epoch epoch{};
  Tick tick{};
  Generation generation{};
  Generation topology_generation{};
  std::uint64_t boot_sequence = 0;
  Digest128 instance{};
  std::string host_label;
};

/// A bundle is the unit of persistence. One bundle is one record, so a torn
/// write can only ever lose a whole commit, never half of one.
struct CommitItem {
  std::string type;
  /// Encoded block without a prefix: "key=value\n" lines.
  std::string payload;
};

struct DecodedCommitItem {
  std::string type;
  TextReader fields;
};

struct DecodedCommit {
  RecordKind kind = RecordKind::Unknown;
  std::vector<DecodedCommitItem> items;
};

class CommitBundle {
 public:
  void add(std::string type, std::string payload);
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] const std::vector<CommitItem>& items() const noexcept { return items_; }
  [[nodiscard]] std::string encode(RecordKind kind) const;
  [[nodiscard]] static Result<DecodedCommit> decode(std::string_view text, const Limits& limits);

 private:
  std::vector<CommitItem> items_;
};

}  // namespace optical_fabric::detail
