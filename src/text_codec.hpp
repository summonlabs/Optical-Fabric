// Optical Fabric 1.0.0 - Summon Software Labs
// Canonical, strict, self-describing text codec used by the store and by the
// control-plane transport.
//
// The codec is strict on purpose: unknown keys, duplicate keys, missing keys,
// unparsable numbers and truncated group counts are all errors. A payload that
// the writer would not have produced is treated as corruption, never as a
// payload to interpret optimistically.
#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/limits.hpp"

namespace optical_fabric::detail {

class TextWriter {
 public:
  /// Sets the prefix applied to every key written until it is popped. Encoders
  /// for nested records rely on this instead of building keys by hand.
  void push_prefix(std::string prefix);
  void pop_prefix();

  /// Writes a bare token line with no key. A payload always begins with one, so
  /// a decoder can tell a real payload from arbitrary bytes.
  void token(std::string_view value);

  void add(std::string_view key, std::string_view value);
  /// Any integral value is written in decimal. A single template keeps the
  /// overload set unambiguous for int, unsigned, size_t and bool arguments.
  template <typename T>
    requires std::is_integral_v<T>
  void add(std::string_view key, T value) {
    add(key, std::to_string(value));
  }
  /// Appends an already-encoded "key=value\n" block, applying a prefix to
  /// every line. Used to nest a record inside a commit bundle.
  void append_block(std::string_view prefix, std::string_view block);

  [[nodiscard]] std::string take();

 private:
  [[nodiscard]] std::string compose(std::string_view key) const;

  std::string text_;
  std::vector<std::string> prefixes_;
};

/// Builds "prefix.index.field".
[[nodiscard]] std::string indexed_key(std::string_view prefix, std::uint64_t index, std::string_view field);

class TextReader {
 public:
  TextReader() = default;

  /// Parses "token\nkey=value\n...". The first line is the token and never
  /// contains '='. Values are unescaped; a value that does not unescape is an
  /// error.
  [[nodiscard]] static Result<TextReader> parse(std::string_view text, const Limits& limits);

  /// Mirrors TextWriter::push_prefix so that nested record decoders can read
  /// their own key namespace.
  void push_prefix_scope(std::string prefix);
  void pop_prefix_scope();

  [[nodiscard]] const std::string& token() const noexcept { return token_; }
  [[nodiscard]] std::size_t field_count() const noexcept { return fields_.size(); }
  [[nodiscard]] bool has(std::string_view key) const;
  [[nodiscard]] std::size_t consumed_count() const noexcept;

  [[nodiscard]] Result<std::string> require_string(std::string_view key);
  [[nodiscard]] Result<std::uint64_t> require_u64(std::string_view key);
  [[nodiscard]] Result<std::int64_t> require_i64(std::string_view key);
  [[nodiscard]] Result<bool> require_bool(std::string_view key);
  [[nodiscard]] Result<std::string> optional_string(std::string_view key, std::string fallback);
  [[nodiscard]] Result<std::uint64_t> optional_u64(std::string_view key, std::uint64_t fallback);

  /// Number of indexed groups under "prefix", stored as "prefix.count".
  [[nodiscard]] Result<std::uint64_t> group_count(std::string_view prefix);
  /// Fields of group "prefix.index" with the prefix stripped, marked consumed.
  [[nodiscard]] Result<TextReader> group(std::string_view prefix, std::uint64_t index);

  /// Marks every remaining field consumed. Used for optional trailing groups.
  void consume_remaining() noexcept;

  /// Returns and consumes every field that has not been read yet, preserving
  /// the order they appeared in.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> take_remaining();

  /// Succeeds only when every field has been consumed by a typed accessor.
  [[nodiscard]] Status finish() const;

 private:
  [[nodiscard]] std::string compose_key(std::string_view key) const;

  std::string token_;
  std::vector<std::pair<std::string, std::string>> fields_;
  std::vector<bool> consumed_;
  std::vector<std::string> prefixes_;
};

/// Strict decimal parsing helpers shared by the codec and request decoding.
[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) noexcept;
[[nodiscard]] bool parse_i64(std::string_view text, std::int64_t& out) noexcept;

}  // namespace optical_fabric::detail
