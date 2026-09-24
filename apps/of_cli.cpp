// Optical Fabric 1.0.0 - Summon Software Labs
// Inspection and demonstration utility.
//
// Every command is read-only with respect to existing state: the inspection
// command parses a store without taking the writer lock, and the demonstration
// command works in a directory it creates.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "optical_fabric/version.hpp"

namespace of = optical_fabric;

namespace {

int inspect(const std::string& path, bool verbose) {
  of::Limits limits;
  const of::Result<of::PersistenceInspection> result =
      of::inspect_persistence(std::filesystem::path(path), limits);
  if (!result.ok()) {
    std::fprintf(stderr, "of_cli: %s: %s\n", path.c_str(), result.status.message.c_str());
    return 1;
  }
  const of::PersistenceInspection& inspection = result.value;
  std::printf("store            %s\n", inspection.path.string().c_str());
  std::printf("exists           %s\n", inspection.exists ? "yes" : "no");
  if (!inspection.exists) {
    std::printf("detail           %s\n", inspection.detail.c_str());
    return 0;
  }
  std::printf("writer_active    %s\n", inspection.locked_by_writer ? "yes" : "no");
  std::printf("file_bytes       %llu\n", static_cast<unsigned long long>(inspection.file_bytes));
  std::printf("format_version   %u\n", inspection.format_version);
  std::printf("header_valid     %s\n", inspection.header_valid ? "yes" : "no");
  std::printf("records          %llu\n", static_cast<unsigned long long>(inspection.records));
  std::printf("checkpoints      %llu\n", static_cast<unsigned long long>(inspection.checkpoints));
  std::printf("valid_bytes      %llu\n", static_cast<unsigned long long>(inspection.valid_bytes));
  std::printf("trailing_bytes   %llu\n", static_cast<unsigned long long>(inspection.trailing_bytes));
  std::printf("tail_torn        %s\n", inspection.tail_torn ? "yes" : "no");
  std::printf("stored_boot      %llu\n", static_cast<unsigned long long>(inspection.stored_boot_sequence));
  std::printf("last_epoch       %llu\n", static_cast<unsigned long long>(inspection.last_epoch));
  std::printf("last_tick        %llu\n", static_cast<unsigned long long>(inspection.last_tick));
  std::printf("content_digest   %s\n", inspection.content_digest.to_string().c_str());
  if (!inspection.detail.empty()) {
    std::printf("detail           %s\n", inspection.detail.c_str());
  }
  if (verbose) {
    std::printf("record kinds:\n");
    for (const of::RecordKindCount& entry : inspection.record_counts) {
      std::printf("  %-24s %llu\n", std::string(of::to_string(entry.kind)).c_str(),
                  static_cast<unsigned long long>(entry.count));
    }
  }
  return 0;
}

/// Runs a complete, synthetic connectivity scenario and reports what the
/// runtime decided. Nothing here talks to optical hardware.
int demonstration(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::fprintf(stderr, "of_cli: cannot create %s\n", directory.string().c_str());
    return 1;
  }
  of::FabricOptions options;
  options.store_path = directory / "fabric.store";
  options.host_label = "of_cli";
  of::OpticalFabric fabric(options);
  if (fabric.closed()) {
    std::fprintf(stderr, "of_cli: %s\n", fabric.recovery().detail.c_str());
    return 1;
  }

  const auto site = [&](const char* name) {
    of::SiteRegistration registration;
    registration.name = name;
    registration.region = "synthetic";
    return of::SiteId::from_value(fabric.register_site(registration).resource.id);
  };
  const of::SiteId site_id = site("demo.site");
  const auto node = [&](const char* name) {
    of::OpticalNodeRegistration registration;
    registration.name = name;
    registration.site = site_id;
    registration.role = "terminal";
    return of::OpticalNodeId::from_value(fabric.register_optical_node(registration).resource.id);
  };
  const of::OpticalNodeId node_a = node("demo.node-a");
  const of::OpticalNodeId node_b = node("demo.node-b");
  const auto port = [&](const char* name, of::OpticalNodeId owner) {
    of::PortRegistration registration;
    registration.name = name;
    registration.node = owner;
    registration.role = "line";
    return of::PortId::from_value(fabric.register_port(registration).resource.id);
  };
  const of::PortId port_a = port("demo.port-a", node_a);
  const of::PortId port_b = port("demo.port-b", node_b);
  const of::PortId client_a = port("demo.client-a", node_a);
  const of::PortId client_b = port("demo.client-b", node_b);
  of::SpanRegistration span;
  span.name = "demo.span";
  span.endpoint_a = port_a;
  span.endpoint_b = port_b;
  span.declared_length_metres = 25000;
  const of::SpanId span_id = of::SpanId::from_value(fabric.register_span(span).resource.id);
  const auto cross = [&](const char* name, of::OpticalNodeId owner, of::PortId ingress, of::PortId egress) {
    of::CrossConnectRegistration registration;
    registration.name = name;
    registration.node = owner;
    registration.ingress = ingress;
    registration.egress = egress;
    return of::CrossConnectId::from_value(fabric.register_cross_connect(registration).resource.id);
  };
  const of::CrossConnectId cross_a = cross("demo.cross-a", node_a, client_a, port_a);
  const of::CrossConnectId cross_b = cross("demo.cross-b", node_b, port_b, client_b);
  of::ChannelRegistration channel;
  channel.name = "demo.channel-0";
  channel.channel_index = 0;
  channel.nominal_frequency_ghz = 191300;
  channel.band = "C";
  const of::ChannelId channel_id = of::ChannelId::from_value(fabric.register_channel(channel).resource.id);

  const std::vector<of::ResourceRef> path_resources = {
      of::as_ref(client_a), of::as_ref(cross_a), of::as_ref(port_a), of::as_ref(span_id),
      of::as_ref(port_b),   of::as_ref(cross_b), of::as_ref(client_b), of::as_ref(channel_id)};
  const std::vector<of::EvidenceKind> kinds = {
      of::EvidenceKind::PortCapability,        of::EvidenceKind::SpanCapability,
      of::EvidenceKind::CrossConnectCapability, of::EvidenceKind::ChannelCapability,
      of::EvidenceKind::PortOperationalState,  of::EvidenceKind::SpanOperationalState,
      of::EvidenceKind::CrossConnectOperationalState, of::EvidenceKind::ChannelAvailability,
      of::EvidenceKind::WavelengthAvailability};
  const of::SourceId source = of::derive_named_id<of::SourceId>("source", "of_cli-synthetic");
  std::uint64_t sequence = 0;
  for (const of::ResourceRef subject : path_resources) {
    for (const of::EvidenceKind kind : kinds) {
      of::EvidenceRecord record;
      record.subject = subject;
      record.kind = kind;
      record.state = of::EvidenceState::Known;
      record.detail = "synthetic observation from the inspection utility";
      record.provenance.source_runtime = "of_cli-synthetic-model";
      record.provenance.source_instance = "of_cli";
      record.provenance.source_id = source;
      record.provenance.source_sequence = ++sequence;
      record.provenance.observed_tick = fabric.current_tick();
      record.provenance.valid_until_tick = fabric.current_tick().advanced_by(4096);
      of::CanonicalHasher hasher;
      hasher.add_field("subject", subject.to_string());
      hasher.add_field("kind", of::to_string(kind));
      record.provenance.content_digest = hasher.digest();
      const of::Result<of::EvidenceId> ingested = fabric.ingest_evidence(record);
      if (!ingested.ok() && ingested.status.code != of::ErrorCode::ConflictingEvidence) {
        std::fprintf(stderr, "of_cli: evidence refused: %s\n", ingested.status.message.c_str());
        return 1;
      }
    }
  }

  of::AuthorityRequest authority;
  authority.attempt = of::generate_attempt_id();
  authority.holder = of::derive_named_id<of::ControllerId>("controller", "of_cli");
  authority.scope = of::AuthorityScope::of_site(site_id);
  authority.lease_ticks = 4096;
  authority.reason = "demonstration";
  const of::AuthorityResult granted = fabric.acquire_authority(authority);
  if (granted.outcome == of::OutcomeCode::Refused) {
    std::fprintf(stderr, "of_cli: authority refused: %s\n", granted.refusal.detail.c_str());
    return 1;
  }

  of::ConnectivityIntent intent;
  intent.name = "demo.client-a-to-client-b";
  intent.owner = "of_cli";
  intent.source_port = client_a;
  intent.destination_port = client_b;
  intent.channel = channel_id;
  intent.reservation_ttl_ticks = 1024;
  const of::IntentSubmission submitted = fabric.submit_intent(intent);
  std::printf("intent           %s\n", std::string(of::to_string(submitted.outcome)).c_str());
  if (submitted.outcome == of::IntentOutcome::Refused) {
    std::fprintf(stderr, "of_cli: %s\n", submitted.refusal.detail.c_str());
    return 1;
  }
  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = granted.token;
  const of::ValidationResult validated = fabric.validate(validation);
  std::printf("validation       %s (%s)\n", std::string(of::to_string(validated.outcome)).c_str(),
              std::string(of::to_string(validated.assessment.aggregate)).c_str());
  if (validated.outcome == of::OutcomeCode::Refused) {
    std::fprintf(stderr, "of_cli: %s\n", validated.refusal.detail.c_str());
    return 1;
  }
  of::ReservationRequest reservation;
  reservation.attempt = of::generate_attempt_id();
  reservation.connectivity = submitted.connectivity;
  reservation.authority = granted.token;
  reservation.ttl_ticks = 1024;
  const of::ReservationResult reserved = fabric.reserve(reservation);
  std::printf("reservation      %s (%llu resources)\n",
              std::string(of::to_string(reserved.outcome)).c_str(),
              static_cast<unsigned long long>(reserved.reservation.resources.size()));
  of::ActivationRequest activation;
  activation.attempt = of::generate_attempt_id();
  activation.connectivity = submitted.connectivity;
  activation.authority = granted.token;
  const of::ActivationResult beginning = fabric.begin_activation(activation);
  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = granted.token;
  commit.activation_digest = beginning.activation_digest;
  const of::ActivationResult activated = fabric.commit_activation(commit);
  std::printf("activation       %s\n", std::string(of::to_string(activated.state)).c_str());

  const of::Result<of::PathExplanation> explanation = fabric.explain(submitted.connectivity);
  if (explanation.ok()) {
    std::printf("path             %s\n", of::render_path_line(
        of::CanonicalPath{explanation.value.canonical_path, explanation.value.identity,
                          static_cast<std::uint32_t>(explanation.value.segments.size())}).c_str());
    std::printf("route origin     %s\n", explanation.value.route_origin.c_str());
    std::printf("evidence         %s\n",
                std::string(of::to_string(explanation.value.evidence_aggregate)).c_str());
    std::printf("authority        %s\n", explanation.value.authority_confirmed ? "confirmed" : "unconfirmed");
    for (const of::SegmentProvenance& segment : explanation.value.segments) {
      std::printf("  segment %-16s gen=%-4s claim=%-24s evidence=%s\n",
                  segment.resource_name.c_str(), segment.registered_generation.to_string().c_str(),
                  segment.claim.c_str(), std::string(of::to_string(segment.evidence_state)).c_str());
    }
  }
  const of::ActivePathReport paths = fabric.active_paths();
  std::printf("active paths     %llu (authorized %llu)\n",
              static_cast<unsigned long long>(paths.active.size()),
              static_cast<unsigned long long>(paths.authorized.size()));
  const of::InvariantReport invariants = fabric.verify_invariants();
  std::printf("invariants       %s (%llu checks)\n", invariants.all_hold ? "hold" : "violated",
              static_cast<unsigned long long>(invariants.checks.size()));
  const of::AccountingReport accounting = fabric.accounting();
  std::printf("accounting       claims=%llu reservations(live=%llu consumed=%llu) balanced=%s\n",
              static_cast<unsigned long long>(accounting.claims),
              static_cast<unsigned long long>(accounting.reservations_live),
              static_cast<unsigned long long>(accounting.reservations_consumed),
              accounting.balanced ? "yes" : "no");
  const of::Status closed = fabric.close();
  if (!closed.ok()) {
    return 1;
  }
  const of::Result<of::PersistenceInspection> inspection =
      of::inspect_persistence(options.store_path, of::Limits{});
  if (inspection.ok()) {
    std::printf("store            %s (%llu records, %llu bytes)\n",
                inspection.value.path.string().c_str(),
                static_cast<unsigned long long>(inspection.value.records),
                static_cast<unsigned long long>(inspection.value.file_bytes));
  }
  return invariants.all_hold && accounting.balanced ? 0 : 1;
}

void usage() {
  std::fprintf(stderr,
               "usage: of_cli <command> [arguments]\n"
               "  inspect <store> [--verbose]   read-only store inspection\n"
               "  demo [directory]              run a synthetic connectivity scenario\n"
               "  version                       print the runtime version\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "version") {
    std::printf("optical fabric %s\n", std::string(of::version_string()).c_str());
    return 0;
  }
  if (command == "inspect") {
    if (argc < 3) {
      usage();
      return 2;
    }
    bool verbose = false;
    for (int index = 3; index < argc; ++index) {
      if (std::string(argv[index]) == "--verbose") {
        verbose = true;
      }
    }
    return inspect(argv[2], verbose);
  }
  if (command == "demo") {
    const std::filesystem::path directory = argc >= 3 ? std::filesystem::path(argv[2])
                                                      : std::filesystem::temp_directory_path() /
                                                            "optical-fabric-demo";
    return demonstration(directory);
  }
  usage();
  return 2;
}
