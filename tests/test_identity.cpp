// Optical Fabric 1.0.0 - Summon Software Labs
// Deterministic identity: names, resources, canonical paths and digests.
#include <set>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"

namespace of = optical_fabric;

OF_TEST(identity, names_are_validated_and_derived_deterministically) {
  OF_REQUIRE(of::Names::valid("edge.site-1", 128));
  OF_REQUIRE(of::Names::valid("a", 128));
  OF_REQUIRE(!of::Names::valid("", 128));
  OF_REQUIRE(!of::Names::valid("-leading", 128));
  OF_REQUIRE(!of::Names::valid("trailing-", 128));
  OF_REQUIRE(!of::Names::valid("has space", 128));
  OF_REQUIRE(!of::Names::valid("has/slash", 128));
  OF_REQUIRE(!of::Names::valid(std::string(129, 'a'), 128));

  const of::SiteId first = of::derive_id<of::SiteId>("edge-a");
  const of::SiteId again = of::derive_id<of::SiteId>("edge-a");
  const of::SiteId other = of::derive_id<of::SiteId>("edge-b");
  OF_REQUIRE_EQ(first, again);
  OF_REQUIRE_NE(first, other);
  OF_REQUIRE(!first.is_nil());

  // The same name in a different kind domain must not collide.
  const of::PortId as_port = of::derive_id<of::PortId>("edge-a");
  OF_REQUIRE_NE(first.value(), as_port.value());

  // Derived identities are stable across parse/format round trips.
  const std::optional<of::SiteId> parsed = of::SiteId::parse(first.to_string());
  OF_REQUIRE(parsed.has_value());
  OF_REQUIRE_EQ(*parsed, first);
  OF_REQUIRE(!of::SiteId::parse("not-hex").has_value());
  OF_REQUIRE(!of::SiteId::parse("").has_value());
  OF_REQUIRE(!of::SiteId::parse("0123456789abcdef0").has_value());

  // A wide sample must be collision free.
  std::set<std::uint64_t> seen;
  for (int index = 0; index < 20000; ++index) {
    const std::string name = "resource-" + std::to_string(index);
    const std::uint64_t value = of::derive_id<of::SiteId>(name).value();
    OF_REQUIRE_MSG(seen.insert(value).second, "identity collision at " + name);
  }
}

OF_TEST(identity, resource_reference_round_trips) {
  const of::ResourceRef ref{of::ResourceKind::Span, 0x1234567890ABCDEFull};
  const std::string text = ref.to_string();
  OF_REQUIRE_EQ(text, std::string("span:1234567890abcdef"));
  const std::optional<of::ResourceRef> parsed = of::ResourceRef::parse(text);
  OF_REQUIRE(parsed.has_value());
  OF_REQUIRE_EQ(*parsed, ref);
  OF_REQUIRE(!of::ResourceRef::parse("nonsense:1").has_value());
  OF_REQUIRE(!of::ResourceRef::parse("span").has_value());
  OF_REQUIRE(!of::ResourceRef::parse("span:zz").has_value());
  OF_REQUIRE(of::as_ref(of::derive_id<of::SpanId>("s1")).kind == of::ResourceKind::Span);
}

OF_TEST(identity, canonical_path_identity_is_deterministic) {
  of::Limits limits;
  of::PathDescriptor path;
  path.channel = of::derive_id<of::ChannelId>("c0");
  path.channel_generation = of::Generation{3};
  path.direction = of::Direction::Forward;
  const auto port = [](const char* name, std::uint64_t generation) {
    return of::PathSegment{of::SegmentRole::Port, of::as_ref(of::derive_id<of::PortId>(name)),
                           of::Generation{generation}};
  };
  path.segments = {port("p1", 1),
                   of::PathSegment{of::SegmentRole::Span, of::as_ref(of::derive_id<of::SpanId>("s1")),
                                   of::Generation{2}},
                   port("p2", 1)};

  const of::Result<of::CanonicalPath> first = of::canonicalize_path(path, limits);
  const of::Result<of::CanonicalPath> second = of::canonicalize_path(path, limits);
  OF_REQUIRE(first.ok());
  OF_REQUIRE(second.ok());
  OF_REQUIRE_EQ(first.value.identity, second.value.identity);
  OF_REQUIRE_EQ(first.value.canonical_text, second.value.canonical_text);
  OF_REQUIRE(of::canonical_path_matches(first.value.canonical_text, first.value.identity));
  OF_REQUIRE_EQ(first.value.segment_count, 3u);

  of::PathDescriptor reordered = path;
  std::swap(reordered.segments[0], reordered.segments[2]);
  const of::Result<of::CanonicalPath> reordered_path = of::canonicalize_path(reordered, limits);
  OF_REQUIRE(reordered_path.ok());
  OF_REQUIRE_NE(reordered_path.value.identity, first.value.identity);

  of::PathDescriptor regenerated = path;
  regenerated.segments[1].generation = of::Generation{9};
  OF_REQUIRE_NE(of::canonicalize_path(regenerated, limits).value.identity, first.value.identity);

  of::PathDescriptor other_channel = path;
  other_channel.channel = of::derive_id<of::ChannelId>("c1");
  OF_REQUIRE_NE(of::canonicalize_path(other_channel, limits).value.identity, first.value.identity);

  of::PathDescriptor reversed = of::reversed_path(path);
  const of::Result<of::CanonicalPath> reversed_path = of::canonicalize_path(reversed, limits);
  OF_REQUIRE(reversed_path.ok());
  OF_REQUIRE_NE(reversed_path.value.identity, first.value.identity);
  OF_REQUIRE(reversed.direction == of::Direction::Reverse);

  // A tampered canonical text must not verify against its identity.
  std::string tampered = first.value.canonical_text;
  tampered.push_back('x');
  OF_REQUIRE(!of::canonical_path_matches(tampered, first.value.identity));
}

OF_TEST(identity, canonical_path_rejects_malformed_input) {
  of::Limits limits;
  of::PathDescriptor empty;
  OF_REQUIRE(!of::canonicalize_path(empty, limits).ok());

  of::PathDescriptor single;
  single.segments.push_back(of::PathSegment{of::SegmentRole::Port,
                                            of::as_ref(of::derive_id<of::PortId>("p1")),
                                            of::Generation{1}});
  OF_REQUIRE(!of::canonicalize_path(single, limits).ok());

  of::PathDescriptor duplicate;
  duplicate.segments.push_back(of::PathSegment{of::SegmentRole::Port,
                                               of::as_ref(of::derive_id<of::PortId>("p1")),
                                               of::Generation{1}});
  duplicate.segments.push_back(duplicate.segments.front());
  OF_REQUIRE(!of::canonicalize_path(duplicate, limits).ok());

  of::PathDescriptor nil_generation;
  nil_generation.segments.push_back(of::PathSegment{of::SegmentRole::Port,
                                                    of::as_ref(of::derive_id<of::PortId>("p1")),
                                                    of::Generation{0}});
  nil_generation.segments.push_back(of::PathSegment{of::SegmentRole::Port,
                                                    of::as_ref(of::derive_id<of::PortId>("p2")),
                                                    of::Generation{1}});
  OF_REQUIRE(!of::canonicalize_path(nil_generation, limits).ok());

  of::Limits tiny;
  tiny.max_path_segments = 1;
  OF_REQUIRE(!of::canonicalize_path(single, tiny).ok());
}

OF_TEST(identity, digest_round_trips_and_crc_detects_change) {
  const of::Digest128 digest = of::CanonicalHasher{}.digest();
  const std::string text = digest.to_string();
  OF_REQUIRE_EQ(text.size(), 32u);
  of::Digest128 parsed{};
  OF_REQUIRE(of::Digest128::parse(text, parsed));
  OF_REQUIRE_EQ(parsed, digest);
  OF_REQUIRE(!of::Digest128::parse("short", parsed));
  OF_REQUIRE(!of::Digest128::parse(std::string(32, 'z'), parsed));

  of::CanonicalHasher hasher;
  hasher.add_field("a", std::string_view("x"));
  hasher.add_field("b", std::string_view("y"));
  of::CanonicalHasher other;
  other.add_field("ab", std::string_view("xy"));
  OF_REQUIRE_NE(hasher.digest(), other.digest());

  of::CanonicalHasher unsigned_field;
  unsigned_field.add_field("n", 7u);
  of::CanonicalHasher sized_field;
  sized_field.add_field("n", static_cast<std::uint64_t>(7));
  OF_REQUIRE_EQ(unsigned_field.digest(), sized_field.digest());

  const std::string payload = "the quick brown fox";
  const std::uint32_t checksum = of::crc32c(payload);
  OF_REQUIRE_EQ(checksum, of::crc32c(payload));
  OF_REQUIRE_NE(checksum, of::crc32c(payload + "x"));
  OF_REQUIRE_NE(checksum, of::crc32c(payload.substr(1)));
}

OF_TEST(identity, scope_and_epoch_arithmetic) {
  const of::SiteId site = of::derive_id<of::SiteId>("s");
  const of::SiteId other = of::derive_id<of::SiteId>("s2");
  const of::AuthorityScope global = of::AuthorityScope::global();
  const of::AuthorityScope local = of::AuthorityScope::of_site(site);
  const of::AuthorityScope stranger = of::AuthorityScope::of_site(other);
  OF_REQUIRE(global.covers(local));
  OF_REQUIRE(global.overlaps(stranger));
  OF_REQUIRE(local.covers(local));
  OF_REQUIRE(!local.covers(stranger));
  OF_REQUIRE(!local.overlaps(stranger));
  OF_REQUIRE(!of::AuthorityScope::none().covers(local));
  OF_REQUIRE(!of::AuthorityScope::none().overlaps(global));

  OF_REQUIRE(of::Epoch{1}.next() == of::Epoch{2});
  OF_REQUIRE(of::Generation{4}.next() == of::Generation{5});
  OF_REQUIRE(of::Tick{10}.advanced_by(5) == of::Tick{15});
  OF_REQUIRE(of::Tick{0} < of::Tick{1});
}
