// Optical Fabric 1.0.0 - Summon Software Labs
// Encoding and decoding of internal state records.
#pragma once

#include <string>

#include "optical_fabric/limits.hpp"
#include "state.hpp"
#include "text_codec.hpp"

namespace optical_fabric::detail {

void encode_resource(TextWriter& writer, const ResourceState& state);
[[nodiscard]] Result<ResourceState> decode_resource(TextReader& reader);

void encode_connectivity(TextWriter& writer, const ConnectivityRecord& record);
[[nodiscard]] Result<ConnectivityRecord> decode_connectivity(TextReader& reader, const Limits& limits);

void encode_evidence(TextWriter& writer, const EvidenceRecord& record);
[[nodiscard]] Result<EvidenceRecord> decode_evidence(TextReader& reader);

void encode_reservation(TextWriter& writer, const ReservationState& state);
[[nodiscard]] Result<ReservationState> decode_reservation(TextReader& reader);

void encode_grant(TextWriter& writer, const GrantState& state);
[[nodiscard]] Result<GrantState> decode_grant(TextReader& reader);

void encode_attempt(TextWriter& writer, const AttemptRecord& record);
[[nodiscard]] Result<AttemptRecord> decode_attempt(TextReader& reader);

void encode_fence(TextWriter& writer, const FenceRecord& record);
[[nodiscard]] Result<FenceRecord> decode_fence(TextReader& reader);

void encode_runtime(TextWriter& writer, const RuntimeState& state);
[[nodiscard]] Result<RuntimeState> decode_runtime(TextReader& reader);

void encode_path(TextWriter& writer, const PathDescriptor& path);
[[nodiscard]] Result<PathDescriptor> decode_path(TextReader& reader);

void encode_authority(TextWriter& writer, const AuthorityToken& token);
[[nodiscard]] Result<AuthorityToken> decode_authority(TextReader& reader);

void encode_transition(TextWriter& writer, const TransitionRecord& record);
[[nodiscard]] Result<TransitionRecord> decode_transition(TextReader& reader);

/// A path descriptor is persisted as its canonical text plus the structural
/// detail; the identity is recomputed on load and a mismatch is corruption.
void encode_canonical(TextWriter& writer, const CanonicalPath& canonical);
[[nodiscard]] Result<CanonicalPath> decode_canonical(TextReader& reader);

}  // namespace optical_fabric::detail
