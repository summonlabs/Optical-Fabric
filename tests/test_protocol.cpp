// Optical Fabric 1.0.0 - Summon Software Labs
// Framed transport: framing, bounds, corruption and message codec.
#include <string>
#include <vector>

#include "optical_fabric/protocol.hpp"
#include "test_harness.hpp"

namespace of = optical_fabric;

OF_TEST(protocol, frames_round_trip_and_survive_partial_feeds) {
  of::Limits limits;
  of::FrameCodec codec(limits);
  const std::string first = codec.encode(7, "first payload");
  const std::string second = codec.encode(8, "second payload");
  const std::string both = first + second;

  std::vector<of::Frame> frames;
  OF_REQUIRE(codec.feed(both.data(), both.size(), frames).ok());
  OF_REQUIRE_EQ(frames.size(), 2u);
  OF_REQUIRE_EQ(frames[0].sequence, 7u);
  OF_REQUIRE_EQ(frames[0].payload, std::string("first payload"));
  OF_REQUIRE_EQ(frames[1].sequence, 8u);
  OF_REQUIRE_EQ(frames[1].payload, std::string("second payload"));

  // Byte at a time: a partial frame must stay buffered, never be delivered.
  of::FrameCodec drip(limits);
  std::size_t delivered = 0;
  for (std::size_t index = 0; index < first.size(); ++index) {
    std::vector<of::Frame> partial;
    OF_REQUIRE(drip.feed(first.data() + index, 1, partial).ok());
    delivered += partial.size();
    if (index + 1 < first.size()) {
      OF_REQUIRE(partial.empty());
    }
  }
  OF_REQUIRE_EQ(delivered, 1u);
  OF_REQUIRE_EQ(drip.pending_bytes(), 0u);
  OF_REQUIRE(!drip.stream_failed());
}

OF_TEST(protocol, malformed_frames_fail_the_stream_before_allocating) {
  of::Limits limits;
  limits.max_frame_payload_bytes = 64;
  const std::string good = of::FrameCodec(limits).encode(1, "payload");

  {
    std::string wrong_magic = good;
    wrong_magic[0] = 'X';
    of::FrameCodec codec(limits);
    std::vector<of::Frame> frames;
    OF_REQUIRE(!codec.feed(wrong_magic.data(), wrong_magic.size(), frames).ok());
    OF_REQUIRE(codec.stream_failed());
  }
  {
    std::string wrong_version = good;
    wrong_version[4] = static_cast<char>(9);
    of::FrameCodec codec(limits);
    std::vector<of::Frame> frames;
    OF_REQUIRE(!codec.feed(wrong_version.data(), wrong_version.size(), frames).ok());
  }
  {
    std::string corrupt = good;
    corrupt[corrupt.size() - 1] = static_cast<char>(corrupt.back() ^ 0x11);
    of::FrameCodec codec(limits);
    std::vector<of::Frame> frames;
    OF_REQUIRE(!codec.feed(corrupt.data(), corrupt.size(), frames).ok());
  }
  {
    // A declared length beyond the bound is refused from the header alone,
    // with no payload present at all.
    std::string oversized = of::FrameCodec(limits).encode(1, std::string(32, 'x'));
    of::Limits small = limits;
    small.max_frame_payload_bytes = 8;
    of::FrameCodec codec(small);
    std::vector<of::Frame> frames;
    OF_REQUIRE(!codec.feed(oversized.data(), oversized.size(), frames).ok());
    OF_REQUIRE_EQ(frames.size(), 0u);
  }
  {
    const of::FrameCodec encoder(limits);
    const std::string header_only = encoder.encode(1, "payload").substr(0, of::kFrameHeaderBytes);
    of::FrameCodec codec(limits);
    std::vector<of::Frame> frames;
    OF_REQUIRE(codec.feed(header_only.data(), header_only.size(), frames).ok());
    OF_REQUIRE(frames.empty());
    OF_REQUIRE(codec.pending_bytes() > 0u);
  }
}

OF_TEST(protocol, messages_round_trip_with_escaping_and_order) {
  of::Limits limits;
  of::ProtocolMessage message;
  message.operation = "submit_intent";
  message.request_id = "req-1";
  message.set("name", "line\nwith\nnewlines");
  message.set("detail", "back\\slash and = sign");
  message.set("empty", "");
  message.set("second", "value");
  const std::string encoded = message.serialize();
  OF_REQUIRE(encoded.find('\n') != std::string::npos);

  const of::Result<of::ProtocolMessage> parsed = of::parse_message(encoded, limits);
  OF_REQUIRE(parsed.ok());
  OF_REQUIRE_EQ(parsed.value.operation, std::string("submit_intent"));
  OF_REQUIRE_EQ(parsed.value.request_id, std::string("req-1"));
  OF_REQUIRE_EQ(parsed.value.get("name"), std::string("line\nwith\nnewlines"));
  OF_REQUIRE_EQ(parsed.value.get("detail"), std::string("back\\slash and = sign"));
  OF_REQUIRE_EQ(parsed.value.get("empty"), std::string());
  OF_REQUIRE_EQ(parsed.value.get("second"), std::string("value"));
  OF_REQUIRE(!parsed.value.has("missing"));
  OF_REQUIRE_EQ(parsed.value.get("missing", "fallback"), std::string("fallback"));
  OF_REQUIRE_EQ(parsed.value.fields.size(), 4u);

  // Re-setting a key replaces it in place, preserving field order.
  of::ProtocolMessage updated = parsed.value;
  updated.set("name", "replaced");
  OF_REQUIRE_EQ(updated.fields.front().second, std::string("replaced"));
  OF_REQUIRE_EQ(updated.fields.size(), 4u);

  // Malformed payloads are refused rather than interpreted.
  OF_REQUIRE(!of::parse_message("", limits).ok());
  OF_REQUIRE(!of::parse_message("op\nno-separator\n", limits).ok());
  OF_REQUIRE(!of::parse_message("op\nkey=value", limits).ok());
  OF_REQUIRE(!of::parse_message("op\nkey=a\nkey=b\n", limits).ok());
  OF_REQUIRE(!of::parse_message("op\nkey=bad\\escape\n", limits).ok());
  OF_REQUIRE(!of::parse_message("op\nrequest_id=1\nkey=trailing\\", limits).ok());
}

OF_TEST(protocol, escaping_is_lossless_for_every_byte_class) {
  const std::vector<std::string> samples = {"", "plain", "with space", "a=b", "line\nbreak",
                                          "carriage\rreturn", "back\\slash", "\\n literal",
                                          "mixed\\and\nboth"};
  for (const std::string& sample : samples) {
    const std::string escaped = of::escape_value(sample);
    std::string restored;
    OF_REQUIRE_MSG(of::unescape_value(escaped, restored), "cannot unescape: " + sample);
    OF_REQUIRE_EQ(restored, sample);
  }
  std::string restored;
  OF_REQUIRE(!of::unescape_value("trailing\\", restored));
  OF_REQUIRE(!of::unescape_value("\\q", restored));
}
