// Optical Fabric 1.0.0 - Summon Software Labs
// Control-plane server and request dispatcher.
#include "optical_fabric/server.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

#include "optical_fabric/version.hpp"
#include "socket.hpp"
#include "state_codec.hpp"
#include "text_codec.hpp"

namespace optical_fabric {

namespace {

using detail::SocketHandle;

void set_field(ProtocolMessage& message, std::string key, std::string value) {
  message.fields.emplace_back(std::move(key), std::move(value));
}

void set_u64(ProtocolMessage& message, std::string key, std::uint64_t value) {
  set_field(message, std::move(key), std::to_string(value));
}

[[nodiscard]] std::uint64_t parse_u64_field(const ProtocolMessage& message, std::string_view key,
                                            std::uint64_t fallback, bool& ok) {
  const std::string* found = message.find(key);
  if (found == nullptr) {
    return fallback;
  }
  std::uint64_t value = 0;
  if (!detail::parse_u64(*found, value)) {
    ok = false;
    return fallback;
  }
  return value;
}

[[nodiscard]] std::uint64_t require_u64(const ProtocolMessage& message, std::string_view key, bool& ok) {
  const std::string* found = message.find(key);
  if (found == nullptr) {
    ok = false;
    return 0;
  }
  std::uint64_t value = 0;
  if (!detail::parse_u64(*found, value)) {
    ok = false;
    return 0;
  }
  return value;
}

/// Accepts either a bare identity or the runtime's own resource reference form
/// ("kind:hex"), so a value taken from one response can be fed straight back
/// into the next request.
template <typename Id>
[[nodiscard]] Id require_id(const ProtocolMessage& message, std::string_view key, bool& ok) {
  const std::string* found = message.find(key);
  if (found == nullptr) {
    ok = false;
    return Id{};
  }
  if (const std::optional<Id> parsed = Id::parse(*found); parsed.has_value()) {
    return *parsed;
  }
  if (const std::optional<ResourceRef> reference = ResourceRef::parse(*found); reference.has_value()) {
    return Id::from_value(reference->id);
  }
  // A decimal form is accepted too, so a caller that formats an identity as a
  // number is understood rather than rejected.
  if (std::uint64_t decimal = 0; !found->empty() && found->size() <= 20 && detail::parse_u64(*found, decimal)) {
    return Id::from_value(decimal);
  }
  ok = false;
  return Id{};
}

[[nodiscard]] ResourceRef require_resource(const ProtocolMessage& message, std::string_view key, bool& ok) {
  const std::string* found = message.find(key);
  if (found == nullptr) {
    ok = false;
    return ResourceRef{};
  }
  const std::optional<ResourceRef> parsed = ResourceRef::parse(*found);
  if (!parsed.has_value()) {
    ok = false;
    return ResourceRef{};
  }
  return *parsed;
}

[[nodiscard]] SharingMode parse_sharing(const ProtocolMessage& message, bool& ok) {
  const std::string value = message.get("sharing", "exclusive");
  if (value == "exclusive") {
    return SharingMode::Exclusive;
  }
  if (value == "channelized") {
    return SharingMode::Channelized;
  }
  ok = false;
  return SharingMode::Exclusive;
}

[[nodiscard]] AuthorityToken parse_authority(const ProtocolMessage& message, bool& ok) {
  AuthorityToken token;
  token.grant = require_id<GrantId>(message, "grant", ok);
  token.holder = require_id<ControllerId>(message, "holder", ok);
  token.epoch = Epoch{require_u64(message, "epoch", ok)};
  token.generation = Generation{parse_u64_field(message, "authority_generation", 0, ok)};
  const std::string scope_kind = message.get("scope_kind", "global");
  if (scope_kind == "global") {
    token.scope = AuthorityScope::global();
  } else if (scope_kind == "site") {
    token.scope = AuthorityScope::of_site(require_id<SiteId>(message, "scope_site", ok));
  } else {
    ok = false;
  }
  token.incarnation.boot_sequence = parse_u64_field(message, "incarnation_boot", 0, ok);
  return token;
}

[[nodiscard]] ProtocolMessage base_response(const ProtocolMessage& request) {
  ProtocolMessage response;
  response.operation = request.operation;
  response.request_id = request.request_id;
  return response;
}

[[nodiscard]] ProtocolMessage failure_response(const ProtocolMessage& request, std::string code,
                                               std::string detail) {
  ProtocolMessage response = base_response(request);
  set_field(response, "status", "refused");
  set_field(response, "refusal", std::move(code));
  set_field(response, "detail", std::move(detail));
  return response;
}

[[nodiscard]] const char* outcome_token(OutcomeCode code) {
  switch (code) {
    case OutcomeCode::Applied: return "applied";
    case OutcomeCode::IdempotentReplay: return "idempotent_replay";
    case OutcomeCode::AlreadySatisfied: return "already_satisfied";
    case OutcomeCode::Refused: return "refused";
  }
  return "refused";
}

void add_outcome(ProtocolMessage& response, OutcomeCode outcome, const Refusal& refusal) {
  set_field(response, "status", outcome == OutcomeCode::Refused ? "refused" : "ok");
  set_field(response, "outcome", outcome_token(outcome));
  set_field(response, "refusal", std::string(to_string(refusal.code)));
  if (!refusal.detail.empty()) {
    set_field(response, "detail", refusal.detail);
  }
}

void add_authority_fields(ProtocolMessage& response, const AuthorityToken& token) {
  set_field(response, "grant", token.grant.to_string());
  set_field(response, "holder", token.holder.to_string());
  set_u64(response, "epoch", token.epoch.value);
  set_u64(response, "authority_generation", token.generation.value);
  set_field(response, "scope_kind", token.scope.kind == ScopeKind::Site ? "site" : "global");
  if (token.scope.kind == ScopeKind::Site) {
    set_field(response, "scope_site", token.scope.site.to_string());
  }
  set_u64(response, "incarnation_boot", token.incarnation.boot_sequence);
}

[[nodiscard]] bool require_kind_field(const ProtocolMessage& message, const char* key, bool fallback) {
  const std::string* found = message.find(key);
  if (found == nullptr) {
    return fallback;
  }
  return *found == "1" || *found == "true";
}

int serve_connection(OpticalFabric& fabric, detail::SocketHandle socket, const Limits& limits,
                     const std::atomic<bool>* stopping) {
  FrameCodec codec(limits);
  std::vector<char> buffer(64 * 1024);
  bool finished = false;
  while (!finished) {
    std::size_t received = 0;
    const Status status = detail::socket_receive(socket, buffer.data(), buffer.size(), received);
    if (!status.ok()) {
      break;
    }
    if (received == 0) {
      break;
    }
    std::vector<Frame> frames;
    const Status fed = codec.feed(buffer.data(), received, frames);
    if (!fed.ok()) {
      break;
    }
    for (const Frame& frame : frames) {
      const Result<ProtocolMessage> request = parse_message(frame.payload, limits);
      if (!request.ok()) {
        break;
      }
      const ProtocolMessage response = dispatch_request(fabric, request.value);
      const std::string encoded = codec.encode(frame.sequence, response.serialize());
      if (!detail::socket_send_all(socket, encoded).ok()) {
        finished = true;
        break;
      }
      if (request.value.operation == "shutdown") {
        finished = true;
        break;
      }
    }
    if (stopping != nullptr && stopping->load()) {
      break;
    }
  }
  detail::socket_shutdown(socket);
  detail::socket_close(socket);
  return 0;
}

}  // namespace

struct FabricNode::Impl {
  std::mutex mutex;
  std::atomic<bool> stopping{false};
  detail::SocketHandle listener = detail::kInvalidSocket;
  std::vector<detail::SocketHandle> clients;
  std::vector<std::thread> workers;
  bool workers_joined = false;

  void join_workers() {
    std::vector<std::thread> pending;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (workers_joined) {
        return;
      }
      workers_joined = true;
      pending.swap(workers);
    }
    for (std::thread& worker : pending) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void shutdown_all() {
    std::lock_guard<std::mutex> guard(mutex);
    detail::socket_shutdown(listener);
    for (const detail::SocketHandle client : clients) {
      detail::socket_shutdown(client);
    }
  }
};

FabricNode::FabricNode(NodeOptions options)
    : impl_(std::make_unique<Impl>()), fabric_(std::make_unique<OpticalFabric>(std::move(options.fabric))) {
  bind_host_ = options.bind_host.empty() ? std::string("127.0.0.1") : options.bind_host;
  port_ = options.port;
  announce_ = options.announce;
  limits_ = options.limits;
  store_label_ = options.fabric.store_path.empty() ? std::string("memory-only")
                                                   : options.fabric.store_path.string();
  impl_->stopping = false;
}

FabricNode::~FabricNode() {
  stop();
  impl_->join_workers();
  if (detail::socket_valid(impl_->listener)) {
    detail::socket_close(impl_->listener);
    impl_->listener = detail::kInvalidSocket;
  }
}

Status FabricNode::start() {
  if (running_.load()) {
    return Status{};
  }
  if (fabric_->closed()) {
    // A runtime that refused its store never announces itself as a control
    // plane: there is no state it could honestly serve.
    return Status::failure(ErrorCode::StoreLocked, fabric_->recovery().detail);
  }
  detail::SocketHandle listener = detail::kInvalidSocket;
  std::uint16_t bound = 0;
  const Status status = detail::socket_listen(bind_host_, port_, listener, bound);
  if (!status.ok()) {
    return status;
  }
  impl_->listener = listener;
  bound_port_ = bound;
  running_.store(true);
  if (announce_) {
    const Incarnation incarnation = fabric_->incarnation();
    std::fprintf(stdout,
                 "optical-fabric node listening host=%s port=%u boot_sequence=%llu instance=%s "
                 "epoch=%llu tick=%llu store=%s\n",
                 bind_host_.c_str(), static_cast<unsigned>(bound),
                 static_cast<unsigned long long>(incarnation.boot_sequence),
                 incarnation.instance.to_string().c_str(),
                 static_cast<unsigned long long>(fabric_->current_epoch().value),
                 static_cast<unsigned long long>(fabric_->current_tick().value),
                 store_label_.c_str());
    std::fflush(stdout);
  }
  return Status{};
}

Status FabricNode::serve() {
  if (!running_.load()) {
    return Status::failure(ErrorCode::Closed, "the node was not started");
  }
  while (!impl_->stopping.load()) {
    detail::SocketHandle client = detail::kInvalidSocket;
    const Status accepted = detail::socket_accept(impl_->listener, client);
    if (!accepted.ok()) {
      break;
    }
    if (impl_->stopping.load()) {
      detail::socket_close(client);
      break;
    }
    bool accepted_connection = false;
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      if (impl_->clients.size() < limits_.max_connections) {
        impl_->clients.push_back(client);
        accepted_connection = true;
      }
    }
    if (!accepted_connection) {
      // The connection bound is a real bound: the extra connection is refused
      // rather than queued.
      detail::socket_close(client);
      continue;
    }
    std::thread worker([this, client]() {
      serve_connection(*fabric_, client, limits_, &impl_->stopping);
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->clients.erase(std::remove(impl_->clients.begin(), impl_->clients.end(), client),
                           impl_->clients.end());
    });
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->workers.push_back(std::move(worker));
  }
  impl_->join_workers();
  running_.store(false);
  return Status{};
}

void FabricNode::stop() {
  impl_->stopping.store(true);
  impl_->shutdown_all();
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

ProtocolMessage dispatch_request(OpticalFabric& fabric, const ProtocolMessage& request) {
  bool ok = true;
  const Limits limits = fabric.limits();
  if (request.operation == "hello") {
    ProtocolMessage response = base_response(request);
    const Incarnation incarnation = fabric.incarnation();
    set_field(response, "status", "ok");
    set_field(response, "version", std::string(version_string()));
    set_u64(response, "boot_sequence", incarnation.boot_sequence);
    set_field(response, "instance", incarnation.instance.to_string());
    set_u64(response, "epoch", fabric.current_epoch().value);
    set_u64(response, "tick", fabric.current_tick().value);
    set_u64(response, "generation", fabric.generation().value);
    set_field(response, "recovery_status", std::string(to_string(fabric.recovery().status)));
    set_field(response, "memory_only", fabric.memory_only() ? "1" : "0");
    return response;
  }
  if (request.operation == "shutdown") {
    ProtocolMessage response = base_response(request);
    set_field(response, "status", "ok");
    set_field(response, "detail", "shutdown accepted");
    return response;
  }
  if (request.operation == "register_site") {
    SiteRegistration registration;
    registration.name = request.get("name");
    registration.region = request.get("region");
    registration.sharing = parse_sharing(request, ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the registration fields are malformed");
    }
    const RegistrationResult result = fabric.register_site(registration);
    ProtocolMessage response = base_response(request);
    set_field(response, "status", result.outcome == TopologyOutcome::Refused ? "refused" : "ok");
    set_field(response, "outcome", std::string(to_string(result.outcome)));
    set_field(response, "refusal", std::string(to_string(result.refusal.code)));
    set_field(response, "resource", result.resource.to_string());
    set_u64(response, "generation", result.generation.value);
    if (!result.refusal.detail.empty()) {
      set_field(response, "detail", result.refusal.detail);
    }
    return response;
  }
  if (request.operation == "register_node" || request.operation == "register_port" ||
      request.operation == "register_span" || request.operation == "register_line_system" ||
      request.operation == "register_cross_connect" || request.operation == "register_channel") {
    ProtocolMessage response = base_response(request);
    RegistrationResult result;
    if (request.operation == "register_node") {
      OpticalNodeRegistration registration;
      registration.name = request.get("name");
      registration.site = require_id<SiteId>(request, "site", ok);
      registration.role = request.get("role");
      registration.sharing = parse_sharing(request, ok);
      if (ok) {
        result = fabric.register_optical_node(registration);
      }
    } else if (request.operation == "register_port") {
      PortRegistration registration;
      registration.name = request.get("name");
      registration.node = require_id<OpticalNodeId>(request, "node", ok);
      registration.role = request.get("role");
      registration.sharing = parse_sharing(request, ok);
      if (ok) {
        result = fabric.register_port(registration);
      }
    } else if (request.operation == "register_span") {
      SpanRegistration registration;
      registration.name = request.get("name");
      registration.endpoint_a = require_id<PortId>(request, "endpoint_a", ok);
      registration.endpoint_b = require_id<PortId>(request, "endpoint_b", ok);
      registration.declared_length_metres = parse_u64_field(request, "length", 0, ok);
      registration.sharing = parse_sharing(request, ok);
      if (ok) {
        result = fabric.register_span(registration);
      }
    } else if (request.operation == "register_line_system") {
      LineSystemRegistration registration;
      registration.name = request.get("name");
      const std::string spans = request.get("spans");
      std::size_t offset = 0;
      while (ok && offset <= spans.size() && !spans.empty()) {
        const std::size_t comma = spans.find(',', offset);
        const std::string token = spans.substr(offset, comma == std::string::npos ? std::string::npos
                                                                                 : comma - offset);
        if (!token.empty()) {
          const std::optional<SpanId> parsed = SpanId::parse(token);
          if (!parsed.has_value()) {
            ok = false;
            break;
          }
          registration.spans.push_back(*parsed);
        }
        if (comma == std::string::npos) {
          break;
        }
        offset = comma + 1;
      }
      const std::string endpoints = request.get("endpoints");
      offset = 0;
      while (ok && offset <= endpoints.size() && !endpoints.empty()) {
        const std::size_t comma = endpoints.find(',', offset);
        const std::string token = endpoints.substr(
            offset, comma == std::string::npos ? std::string::npos : comma - offset);
        if (!token.empty()) {
          const std::optional<PortId> parsed = PortId::parse(token);
          if (!parsed.has_value()) {
            ok = false;
            break;
          }
          registration.endpoints.push_back(*parsed);
        }
        if (comma == std::string::npos) {
          break;
        }
        offset = comma + 1;
      }
      registration.sharing = parse_sharing(request, ok);
      if (ok) {
        result = fabric.register_line_system(registration);
      }
    } else if (request.operation == "register_cross_connect") {
      CrossConnectRegistration registration;
      registration.name = request.get("name");
      registration.node = require_id<OpticalNodeId>(request, "node", ok);
      registration.ingress = require_id<PortId>(request, "ingress", ok);
      registration.egress = require_id<PortId>(request, "egress", ok);
      registration.sharing = parse_sharing(request, ok);
      if (ok) {
        result = fabric.register_cross_connect(registration);
      }
    } else {
      ChannelRegistration registration;
      registration.name = request.get("name");
      const std::uint64_t index = parse_u64_field(request, "index", 0, ok);
      registration.channel_index = static_cast<std::uint32_t>(index);
      registration.nominal_frequency_ghz = parse_u64_field(request, "frequency", 0, ok);
      registration.band = request.get("band");
      registration.sharing = parse_sharing(request, ok);
      if (ok) {
        result = fabric.register_channel(registration);
      }
    }
    if (!ok) {
      return failure_response(request, "invalid_argument", "the registration fields are malformed");
    }
    set_field(response, "status", result.outcome == TopologyOutcome::Refused ? "refused" : "ok");
    set_field(response, "outcome", std::string(to_string(result.outcome)));
    set_field(response, "refusal", std::string(to_string(result.refusal.code)));
    set_field(response, "resource", result.resource.to_string());
    set_u64(response, "generation", result.generation.value);
    if (!result.refusal.detail.empty()) {
      set_field(response, "detail", result.refusal.detail);
    }
    return response;
  }
  if (request.operation == "ingest_evidence") {
    const std::string* block = request.find("record");
    if (block == nullptr) {
      return failure_response(request, "invalid_argument", "the evidence record is missing");
    }
    const Result<detail::TextReader> parsed = detail::TextReader::parse(*block, limits);
    if (!parsed.ok()) {
      return failure_response(request, "invalid_argument", parsed.status.message);
    }
    detail::TextReader reader = parsed.value;
    const Result<EvidenceRecord> record = detail::decode_evidence(reader);
    if (!record.ok()) {
      return failure_response(request, "invalid_argument", record.status.message);
    }
    const Result<EvidenceId> ingested = fabric.ingest_evidence(record.value);
    ProtocolMessage response = base_response(request);
    if (!ingested.ok()) {
      set_field(response, "status", "refused");
      set_field(response, "refusal", std::string(to_string(ingested.status.code)));
      set_field(response, "detail", ingested.status.message);
      return response;
    }
    set_field(response, "status", "ok");
    set_field(response, "evidence", ingested.value.to_string());
    return response;
  }
  if (request.operation == "ingest_observation") {
    // A remote producer states one observation. The runtime fills in the tick,
    // the ingestion incarnation and the content digest itself, so a producer
    // cannot claim to have been observed later than it was.
    EvidenceRecord record;
    record.subject = require_resource(request, "subject", ok);
    const std::string kind_token = request.get("kind");
    if (!evidence_kind_from_string(kind_token, record.kind)) {
      ok = false;
    }
    const std::string state_token = request.get("state", "known");
    if (!evidence_state_from_string(state_token, record.state)) {
      ok = false;
    }
    record.advisory = require_kind_field(request, "advisory", false);
    record.detail = request.get("detail");
    record.provenance.source_runtime = request.get("runtime");
    record.provenance.source_instance = request.get("instance");
    record.provenance.source_id = require_id<SourceId>(request, "source", ok);
    const std::uint64_t sequence = require_u64(request, "sequence", ok);
    const std::uint64_t span = require_u64(request, "validity", ok);
    if (span == 0) {
      ok = false;
    }
    if (!ok) {
      return failure_response(request, "invalid_argument", "the observation fields are malformed");
    }
    record.provenance.source_sequence = sequence;
    record.provenance.observed_tick = fabric.current_tick();
    record.provenance.valid_until_tick = fabric.current_tick().advanced_by(span);
    CanonicalHasher hasher;
    hasher.add_field("runtime", record.provenance.source_runtime);
    hasher.add_field("subject", record.subject.to_string());
    hasher.add_field("kind", to_string(record.kind));
    hasher.add_field("state", to_string(record.state));
    hasher.add_field("sequence", record.provenance.source_sequence);
    record.provenance.content_digest = hasher.digest();
    const Result<EvidenceId> ingested = fabric.ingest_evidence(record);
    ProtocolMessage response = base_response(request);
    if (!ingested.ok()) {
      set_field(response, "status", "refused");
      set_field(response, "refusal", std::string(to_string(ingested.status.code)));
      set_field(response, "detail", ingested.status.message);
      return response;
    }
    set_field(response, "status", "ok");
    set_field(response, "evidence", ingested.value.to_string());
    return response;
  }
  if (request.operation == "describe_resource") {
    const ResourceRef resource = require_resource(request, "resource", ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the resource reference is malformed");
    }
    const Result<ResourceView> view = fabric.describe_resource(resource);
    ProtocolMessage response = base_response(request);
    if (!view.ok()) {
      return failure_response(request, std::string(to_string(view.status.code)), view.status.message);
    }
    set_field(response, "status", "ok");
    set_field(response, "name", view.value.name);
    set_u64(response, "generation", view.value.generation.value);
    set_field(response, "sharing", std::string(to_string(view.value.sharing)));
    return response;
  }
  if (request.operation == "acquire_authority" || request.operation == "renew_authority" ||
      request.operation == "fence") {
    ControllerId holder = require_id<ControllerId>(request, "holder", ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the authority holder is missing");
    }
    const std::string scope_kind = request.get("scope_kind", "global");
    AuthorityScope scope = AuthorityScope::global();
    if (scope_kind == "site") {
      scope = AuthorityScope::of_site(require_id<SiteId>(request, "scope_site", ok));
    } else if (scope_kind != "global") {
      ok = false;
    }
    if (!ok) {
      return failure_response(request, "invalid_argument", "the authority scope is malformed");
    }
    const std::uint64_t lease = parse_u64_field(request, "lease", 256, ok);
    const AttemptId attempt = require_id<AttemptId>(request, "attempt", ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the authority request is malformed");
    }
    ProtocolMessage response = base_response(request);
    if (request.operation == "fence") {
      FenceRequest fence;
      fence.attempt = attempt;
      fence.holder = holder;
      fence.scope = scope;
      fence.lease_ticks = lease;
      fence.detail = request.get("detail");
      const std::string reason = request.get("reason", "explicit_operator_fence");
      static constexpr std::array<FenceReason, 6> reasons = {
          FenceReason::ExplicitOperatorFence, FenceReason::SupersededByNewerEpoch,
          FenceReason::SupersededByNewerIncarnation, FenceReason::StoreTakeover,
          FenceReason::AuthorityReleased, FenceReason::ScopeRevoked};
      bool matched = false;
      for (const FenceReason candidate : reasons) {
        if (to_string(candidate) == reason) {
          fence.reason = candidate;
          matched = true;
          break;
        }
      }
      if (!matched) {
        return failure_response(request, "invalid_argument", "the fence reason is unknown");
      }
      const FenceResult result = fabric.fence(fence);
      add_outcome(response, result.outcome, result.refusal);
      add_authority_fields(response, result.token);
      set_field(response, "fenced", result.fenced_anything ? "1" : "0");
      set_field(response, "fenced_grant", result.fenced.grant.to_string());
      set_u64(response, "fenced_boot", result.fenced.fenced_boot_sequence);
      set_field(response, "fence_reason", std::string(to_string(result.fenced.reason)));
      return response;
    }
    AuthorityRequest authority;
    authority.attempt = attempt;
    authority.holder = holder;
    authority.scope = scope;
    authority.lease_ticks = lease;
    authority.reason = request.get("reason");
    const AuthorityResult result = request.operation == "renew_authority"
                                       ? fabric.renew_authority(authority)
                                       : fabric.acquire_authority(authority);
    add_outcome(response, result.outcome, result.refusal);
    add_authority_fields(response, result.token);
    set_u64(response, "expiry_tick", result.grant.expiry_tick.value);
    return response;
  }
  if (request.operation == "submit_intent") {
    ConnectivityIntent intent;
    intent.name = request.get("name");
    intent.owner = request.get("owner");
    intent.source_port = require_id<PortId>(request, "source", ok);
    intent.destination_port = require_id<PortId>(request, "destination", ok);
    if (request.has("channel")) {
      intent.channel = require_id<ChannelId>(request, "channel", ok);
    }
    const std::string direction = request.get("direction", "forward");
    if (!direction_from_string(direction, intent.direction)) {
      ok = false;
    }
    intent.requirements.require_capability_evidence = require_kind_field(request, "require_capability", true);
    intent.requirements.require_operational_evidence =
        require_kind_field(request, "require_operational", true);
    intent.requirements.require_attachment_evidence =
        require_kind_field(request, "require_attachment", false);
    intent.reservation_ttl_ticks = parse_u64_field(request, "ttl", 64, ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the intent fields are malformed");
    }
    const IntentSubmission result = fabric.submit_intent(intent);
    ProtocolMessage response = base_response(request);
    set_field(response, "status", result.outcome == IntentOutcome::Refused ? "refused" : "ok");
    set_field(response, "outcome", std::string(to_string(result.outcome)));
    set_field(response, "refusal", std::string(to_string(result.refusal.code)));
    if (!result.refusal.detail.empty()) {
      set_field(response, "detail", result.refusal.detail);
    }
    set_field(response, "connectivity", result.connectivity.to_string());
    set_field(response, "path_identity", result.path_identity.to_string());
    set_field(response, "state", std::string(to_string(result.state)));
    set_u64(response, "generation", result.generation.value);
    set_field(response, "created", result.created ? "1" : "0");
    return response;
  }
  if (request.operation == "advance_tick") {
    const std::uint64_t delta = require_u64(request, "delta", ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the tick delta is missing");
    }
    const Result<Tick> advanced = fabric.advance_tick(delta);
    ProtocolMessage response = base_response(request);
    if (!advanced.ok()) {
      set_field(response, "status", "refused");
      set_field(response, "refusal", std::string(to_string(advanced.status.code)));
      set_field(response, "detail", advanced.status.message);
      return response;
    }
    set_field(response, "status", "ok");
    set_u64(response, "tick", advanced.value.value);
    return response;
  }
  if (request.operation == "validate") {
    ValidateRequest operation;
    operation.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const ValidationResult result = fabric.validate(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_field(response, "state", std::string(to_string(result.state)));
    set_u64(response, "generation", result.generation.value);
    set_field(response, "evidence", std::string(to_string(result.assessment.aggregate)));
    return response;
  }
  if (request.operation == "reserve") {
    ReservationRequest operation;
    operation.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    operation.ttl_ticks = parse_u64_field(request, "ttl", 64, ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const ReservationResult result = fabric.reserve(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_field(response, "reservation", result.reservation.id.to_string());
    set_u64(response, "expiry_tick", result.reservation.expiry_tick.value);
    set_u64(response, "generation", result.generation.value);
    set_u64(response, "resources", result.reservation.resources.size());
    return response;
  }
  if (request.operation == "renew_reservation") {
    ReservationRenewal operation;
    operation.reservation = require_id<ReservationId>(request, "reservation", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    operation.extend_ticks = parse_u64_field(request, "extend", 64, ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const ReservationResult result = fabric.renew_reservation(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_u64(response, "expiry_tick", result.reservation.expiry_tick.value);
    return response;
  }
  if (request.operation == "release_reservation") {
    ReleaseRequest operation;
    operation.reservation = require_id<ReservationId>(request, "reservation", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    operation.reason = request.get("reason");
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const ReleaseResult result = fabric.release_reservation(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_u64(response, "released", result.released.size());
    return response;
  }
  if (request.operation == "begin_activation") {
    ActivationRequest operation;
    operation.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const ActivationResult result = fabric.begin_activation(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_field(response, "state", std::string(to_string(result.state)));
    set_field(response, "activation_digest", result.activation_digest.to_string());
    set_u64(response, "generation", result.generation.value);
    set_field(response, "evidence", std::string(to_string(result.assessment.aggregate)));
    return response;
  }
  if (request.operation == "commit_activation") {
    CommitRequest operation;
    operation.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    const std::string digest = request.get("activation_digest");
    if (ok && !Digest128::parse(digest, operation.activation_digest)) {
      ok = false;
    }
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const ActivationResult result = fabric.commit_activation(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_field(response, "state", std::string(to_string(result.state)));
    set_u64(response, "generation", result.generation.value);
    set_u64(response, "activated_tick", result.activated_tick.value);
    set_u64(response, "committed", result.committed.size());
    return response;
  }
  if (request.operation == "withdraw") {
    WithdrawalRequest operation;
    operation.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
    operation.attempt = require_id<AttemptId>(request, "attempt", ok);
    operation.authority = parse_authority(request, ok);
    operation.reason = request.get("reason");
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const WithdrawalResult result = fabric.withdraw(operation);
    ProtocolMessage response = base_response(request);
    add_outcome(response, result.outcome, result.refusal);
    set_field(response, "state", std::string(to_string(result.state)));
    set_u64(response, "generation", result.generation.value);
    set_u64(response, "released", result.released.size());
    set_field(response, "terminal", result.terminal ? "1" : "0");
    return response;
  }
  if (request.operation == "revalidate" || request.operation == "report_degraded" ||
      request.operation == "report_failed") {
    ProtocolMessage response = base_response(request);
    if (request.operation == "revalidate") {
      RevalidationRequest operation;
      operation.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
      operation.attempt = require_id<AttemptId>(request, "attempt", ok);
      operation.authority = parse_authority(request, ok);
      operation.withdraw_on_failure = require_kind_field(request, "withdraw_on_failure", true);
      if (!ok) {
        return failure_response(request, "invalid_argument", "the request fields are malformed");
      }
      const RevalidationResult result = fabric.revalidate(operation);
      add_outcome(response, result.outcome, result.refusal);
      set_field(response, "state", std::string(to_string(result.state)));
      set_u64(response, "generation", result.generation.value);
      set_field(response, "authority_confirmed", result.authority_confirmed ? "1" : "0");
      set_field(response, "evidence", std::string(to_string(result.assessment.aggregate)));
      return response;
    }
    HealthReport report;
    report.connectivity = require_id<ConnectivityId>(request, "connectivity", ok);
    report.attempt = require_id<AttemptId>(request, "attempt", ok);
    report.authority = parse_authority(request, ok);
    report.detail = request.get("detail");
    const std::string observed = request.get("observed", "known");
    if (!evidence_state_from_string(observed, report.observed)) {
      ok = false;
    }
    if (!ok) {
      return failure_response(request, "invalid_argument", "the request fields are malformed");
    }
    const HealthResult result = request.operation == "report_degraded" ? fabric.report_degraded(report)
                                                                      : fabric.report_failed(report);
    add_outcome(response, result.outcome, result.refusal);
    set_field(response, "state", std::string(to_string(result.state)));
    set_u64(response, "generation", result.generation.value);
    return response;
  }
  if (request.operation == "describe_connectivity" || request.operation == "explain") {
    const ConnectivityId id = require_id<ConnectivityId>(request, "connectivity", ok);
    if (!ok) {
      return failure_response(request, "invalid_argument", "the connectivity identity is missing");
    }
    ProtocolMessage response = base_response(request);
    if (request.operation == "describe_connectivity") {
      const Result<ConnectivityView> view = fabric.describe_connectivity(id);
      if (!view.ok()) {
        return failure_response(request, std::string(to_string(view.status.code)), view.status.message);
      }
      set_field(response, "status", "ok");
      set_field(response, "state", std::string(to_string(view.value.state)));
      set_u64(response, "generation", view.value.generation.value);
      set_field(response, "path_identity", view.value.path_identity.to_string());
      set_field(response, "authority_confirmed", view.value.authority_confirmed ? "1" : "0");
      set_u64(response, "activated_tick", view.value.activated_tick.value);
      set_field(response, "refusal", std::string(to_string(view.value.last_refusal)));
      set_field(response, "segments", view.value.canonical_path);
      return response;
    }
    const Result<PathExplanation> explanation = fabric.explain(id);
    if (!explanation.ok()) {
      return failure_response(request, std::string(to_string(explanation.status.code)),
                              explanation.status.message);
    }
    set_field(response, "status", "ok");
    set_field(response, "state", std::string(to_string(explanation.value.state)));
    set_field(response, "path_identity", explanation.value.identity.to_string());
    set_field(response, "evidence", std::string(to_string(explanation.value.evidence_aggregate)));
    set_field(response, "authority_confirmed", explanation.value.authority_confirmed ? "1" : "0");
    set_u64(response, "segments", explanation.value.segments.size());
    set_u64(response, "history", explanation.value.history.size());
    return response;
  }
  if (request.operation == "active_paths") {
    const ActivePathReport report = fabric.active_paths();
    ProtocolMessage response = base_response(request);
    set_field(response, "status", "ok");
    set_u64(response, "active", report.active.size());
    set_u64(response, "authorized", report.authorized.size());
    set_u64(response, "diagnostics", report.diagnostics.size());
    set_field(response, "digest", report.digest.to_string());
    for (std::size_t index = 0; index < report.active.size(); ++index) {
      const ActivePathEntry& entry = report.active[index];
      const std::string prefix = "entry." + std::to_string(index) + ".";
      set_field(response, prefix + "connectivity", entry.connectivity.to_string());
      set_field(response, prefix + "state", std::string(to_string(entry.state)));
      set_field(response, prefix + "authorized", entry.authorized ? "1" : "0");
      set_field(response, prefix + "path_identity", entry.identity.to_string());
      set_field(response, prefix + "reason", entry.unauthorized_reason);
    }
    return response;
  }
  if (request.operation == "snapshot") {
    const FabricSnapshot snapshot = fabric.snapshot();
    ProtocolMessage response = base_response(request);
    set_field(response, "status", "ok");
    set_u64(response, "epoch", snapshot.epoch.value);
    set_u64(response, "tick", snapshot.tick.value);
    set_u64(response, "generation", snapshot.generation.value);
    set_u64(response, "topology_generation", snapshot.topology_generation.value);
    set_u64(response, "boot_sequence", snapshot.incarnation.boot_sequence);
    set_field(response, "instance", snapshot.incarnation.instance.to_string());
    set_u64(response, "sites", snapshot.sites);
    set_u64(response, "ports", snapshot.ports);
    set_u64(response, "spans", snapshot.spans);
    set_u64(response, "cross_connects", snapshot.cross_connects);
    set_u64(response, "channels", snapshot.channels);
    set_u64(response, "objects", snapshot.connectivity_objects);
    set_u64(response, "active", snapshot.connectivity_active);
    set_u64(response, "reservations", snapshot.reservations);
    set_u64(response, "claims", snapshot.claims);
    set_u64(response, "evidence", snapshot.evidence_records);
    set_u64(response, "grants", snapshot.authority_grants);
    set_field(response, "digest", snapshot.digest.to_string());
    return response;
  }
  if (request.operation == "accounting") {
    const AccountingReport report = fabric.accounting();
    ProtocolMessage response = base_response(request);
    set_field(response, "status", "ok");
    set_u64(response, "resources", report.resources_registered);
    set_u64(response, "claims", report.claims);
    set_u64(response, "reservations_live", report.reservations_live);
    set_u64(response, "reservations_consumed", report.reservations_consumed);
    set_u64(response, "reservations_released", report.reservations_released);
    set_u64(response, "objects", report.objects_total);
    set_u64(response, "grants_live", report.grants_live);
    set_u64(response, "grants_fenced", report.grants_fenced);
    set_u64(response, "evidence", report.evidence_records);
    set_u64(response, "evidence_stale", report.evidence_stale);
    set_field(response, "balanced", report.balanced ? "1" : "0");
    return response;
  }
  if (request.operation == "verify_invariants") {
    const InvariantReport report = fabric.verify_invariants();
    ProtocolMessage response = base_response(request);
    set_field(response, "status", "ok");
    set_field(response, "all_hold", report.all_hold ? "1" : "0");
    set_u64(response, "checks", report.checks.size());
    set_u64(response, "failures", report.failures());
    for (std::size_t index = 0; index < report.checks.size(); ++index) {
      const InvariantCheck& check = report.checks[index];
      const std::string prefix = "check." + std::to_string(index) + ".";
      set_field(response, prefix + "name", check.name);
      set_field(response, prefix + "holds", check.holds ? "1" : "0");
      set_field(response, prefix + "detail", check.detail);
    }
    return response;
  }
  return failure_response(request, "unsupported_boundary",
                          "the operation is not part of the control-plane surface");
}

}  // namespace optical_fabric
