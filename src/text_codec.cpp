// Optical Fabric 1.0.0 - Summon Software Labs
#include "text_codec.hpp"

#include <algorithm>
#include <limits>

#include "optical_fabric/protocol.hpp"

namespace optical_fabric {

/// Field escaping is shared by the transport and the store. Values may contain
/// any byte except NUL; newlines and backslashes are escaped.
std::string escape_value(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      default: out.push_back(character); break;
    }
  }
  return out;
}

bool unescape_value(std::string_view text, std::string& out) noexcept {
  out.clear();
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      out.push_back(character);
      continue;
    }
    if (index + 1 >= text.size()) {
      return false;
    }
    const char escaped = text[++index];
    switch (escaped) {
      case '\\': out.push_back('\\'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      default: return false;
    }
  }
  return true;
}

}  // namespace optical_fabric

namespace optical_fabric::detail {


void TextWriter::token(std::string_view value) {
  text_.append(value);
  text_.push_back('\n');
}

void TextWriter::push_prefix(std::string prefix) { prefixes_.push_back(std::move(prefix)); }

void TextWriter::pop_prefix() {
  if (!prefixes_.empty()) {
    prefixes_.pop_back();
  }
}

std::string TextWriter::compose(std::string_view key) const {
  if (prefixes_.empty()) {
    return std::string(key);
  }
  std::string out;
  for (const std::string& prefix : prefixes_) {
    out.append(prefix);
  }
  out.append(key);
  return out;
}

void TextWriter::add(std::string_view key, std::string_view value) {
  const std::string composed = compose(key);
  text_.append(composed);
  text_.push_back('=');
  text_.append(escape_value(value));
  text_.push_back('\n');
}


void TextWriter::append_block(std::string_view prefix, std::string_view block) {
  std::size_t offset = 0;
  while (offset < block.size()) {
    const std::size_t newline = block.find('\n', offset);
    if (newline == std::string_view::npos) {
      text_.append(prefix);
      text_.append(block.substr(offset));
      text_.push_back('\n');
      break;
    }
    if (newline > offset) {
      text_.append(prefix);
      text_.append(block.substr(offset, newline - offset));
      text_.push_back('\n');
    }
    offset = newline + 1;
  }
}

std::string TextWriter::take() { return std::move(text_); }

std::string indexed_key(std::string_view prefix, std::uint64_t index, std::string_view field) {
  std::string key;
  key.reserve(prefix.size() + field.size() + 24);
  key.append(prefix);
  key.push_back('.');
  key.append(std::to_string(index));
  if (!field.empty()) {
    key.push_back('.');
    key.append(field);
  }
  return key;
}

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    const std::uint64_t next = value * 10u + static_cast<std::uint64_t>(digit - '0');
    if (next < value) {
      return false;
    }
    value = next;
  }
  out = value;
  return true;
}

bool parse_i64(std::string_view text, std::int64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  bool negative = false;
  std::string_view digits = text;
  if (digits.front() == '-') {
    negative = true;
    digits.remove_prefix(1);
  } else if (digits.front() == '+') {
    digits.remove_prefix(1);
  }
  std::uint64_t magnitude = 0;
  if (!parse_u64(digits, magnitude)) {
    return false;
  }
  if (negative) {
    if (magnitude > 9223372036854775808ull) {
      return false;
    }
    out = magnitude == 9223372036854775808ull ? std::numeric_limits<std::int64_t>::min()
                                              : -static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude > 9223372036854775807ull) {
    return false;
  }
  out = static_cast<std::int64_t>(magnitude);
  return true;
}

Result<TextReader> TextReader::parse(std::string_view text, const Limits& limits) {
  TextReader reader;
  std::size_t offset = 0;
  bool first = true;
  while (offset <= text.size()) {
    if (offset == text.size()) {
      break;
    }
    const std::size_t newline = text.find('\n', offset);
    if (newline == std::string_view::npos) {
      return Result<TextReader>::failure(ErrorCode::InvalidArgument,
                                         "text payload has no terminating newline");
    }
    const std::string_view line = text.substr(offset, newline - offset);
    offset = newline + 1;
    if (first) {
      first = false;
      if (line.empty() || line.find('=') != std::string_view::npos) {
        return Result<TextReader>::failure(ErrorCode::InvalidArgument,
                                           "text payload does not begin with a token line");
      }
      reader.token_.assign(line);
      continue;
    }
    if (line.empty()) {
      continue;
    }
    if (reader.fields_.size() >= 200000) {
      return Result<TextReader>::failure(ErrorCode::CapacityExceeded,
                                         "text payload exceeds the field bound");
    }
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos || separator == 0) {
      return Result<TextReader>::failure(ErrorCode::InvalidArgument,
                                         "text payload contains a line without a key");
    }
    std::string key(line.substr(0, separator));
    std::string value;
    if (!unescape_value(line.substr(separator + 1), value)) {
      return Result<TextReader>::failure(ErrorCode::InvalidArgument,
                                         "text payload contains an invalid escape sequence");
    }
    for (const auto& field : reader.fields_) {
      if (field.first == key) {
        return Result<TextReader>::failure(ErrorCode::InvalidArgument,
                                           "text payload repeats the key: " + key);
      }
    }
    reader.fields_.emplace_back(std::move(key), std::move(value));
  }
  if (reader.token_.empty()) {
    return Result<TextReader>::failure(ErrorCode::InvalidArgument, "text payload is empty");
  }
  if (reader.fields_.size() > limits.max_journal_records) {
    return Result<TextReader>::failure(ErrorCode::CapacityExceeded,
                                       "text payload exceeds the configured field bound");
  }
  reader.consumed_.assign(reader.fields_.size(), false);
  return Result<TextReader>::success(std::move(reader));
}

void TextReader::push_prefix_scope(std::string prefix) { prefixes_.push_back(std::move(prefix)); }

void TextReader::pop_prefix_scope() {
  if (!prefixes_.empty()) {
    prefixes_.pop_back();
  }
}

std::string TextReader::compose_key(std::string_view key) const {
  if (prefixes_.empty()) {
    return std::string(key);
  }
  std::string out;
  for (const std::string& prefix : prefixes_) {
    out.append(prefix);
  }
  out.append(key);
  return out;
}

bool TextReader::has(std::string_view key) const {
  const std::string composed = compose_key(key);
  for (const auto& field : fields_) {
    if (field.first == composed) {
      return true;
    }
  }
  return false;
}

std::size_t TextReader::consumed_count() const noexcept {
  return static_cast<std::size_t>(std::count(consumed_.begin(), consumed_.end(), true));
}

namespace {

[[nodiscard]] const std::pair<std::string, std::string>* lookup(
    const std::vector<std::pair<std::string, std::string>>& fields, std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      return &field;
    }
  }
  return nullptr;
}

}  // namespace

Result<std::string> TextReader::require_string(std::string_view key) {
  const std::string composed = compose_key(key);
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    if (fields_[index].first == composed) {
      consumed_[index] = true;
      return Result<std::string>::success(fields_[index].second);
    }
  }
  return Result<std::string>::failure(ErrorCode::InvalidArgument,
                                      "text payload is missing the key: " + composed);
}

Result<std::uint64_t> TextReader::require_u64(std::string_view key) {
  const Result<std::string> raw = require_string(key);
  if (!raw.ok()) {
    return Result<std::uint64_t>::failure(raw.status.code, raw.status.message);
  }
  std::uint64_t value = 0;
  if (!parse_u64(raw.value, value)) {
    return Result<std::uint64_t>::failure(ErrorCode::InvalidArgument,
                                          "text payload has a non-decimal value for: " + std::string(key));
  }
  return Result<std::uint64_t>::success(value);
}

Result<std::int64_t> TextReader::require_i64(std::string_view key) {
  const Result<std::string> raw = require_string(key);
  if (!raw.ok()) {
    return Result<std::int64_t>::failure(raw.status.code, raw.status.message);
  }
  std::int64_t value = 0;
  if (!parse_i64(raw.value, value)) {
    return Result<std::int64_t>::failure(ErrorCode::InvalidArgument,
                                         "text payload has a non-integer value for: " + std::string(key));
  }
  return Result<std::int64_t>::success(value);
}

Result<bool> TextReader::require_bool(std::string_view key) {
  const Result<std::uint64_t> raw = require_u64(key);
  if (!raw.ok()) {
    return Result<bool>::failure(raw.status.code, raw.status.message);
  }
  if (raw.value > 1) {
    return Result<bool>::failure(ErrorCode::InvalidArgument,
                                 "text payload has a non-boolean value for: " + std::string(key));
  }
  return Result<bool>::success(raw.value == 1);
}

Result<std::string> TextReader::optional_string(std::string_view key, std::string fallback) {
  if (!has(key)) {
    return Result<std::string>::success(std::move(fallback));
  }
  return require_string(key);
}

void TextReader::consume_remaining() noexcept {
  for (std::size_t index = 0; index < consumed_.size(); ++index) {
    consumed_[index] = true;
  }
}

Result<std::uint64_t> TextReader::optional_u64(std::string_view key, std::uint64_t fallback) {
  if (!has(key)) {
    return Result<std::uint64_t>::success(fallback);
  }
  return require_u64(key);
}

Result<std::uint64_t> TextReader::group_count(std::string_view prefix) {
  return require_u64(std::string(prefix) + ".count");
}

Result<TextReader> TextReader::group(std::string_view prefix, std::uint64_t index) {
  const std::string group_prefix = compose_key(prefix) + "." + std::to_string(index) + ".";
  TextReader sub;
  sub.token_ = std::string(prefix);
  for (std::size_t position = 0; position < fields_.size(); ++position) {
    const std::string& key = fields_[position].first;
    if (key.size() <= group_prefix.size() || key.compare(0, group_prefix.size(), group_prefix) != 0) {
      continue;
    }
    sub.fields_.emplace_back(key.substr(group_prefix.size()), fields_[position].second);
    consumed_[position] = true;
  }
  if (sub.fields_.empty()) {
    return Result<TextReader>::failure(ErrorCode::InvalidArgument,
                                       "text payload is missing the group: " + group_prefix);
  }
  sub.consumed_.assign(sub.fields_.size(), false);
  return Result<TextReader>::success(std::move(sub));
}

std::vector<std::pair<std::string, std::string>> TextReader::take_remaining() {
  std::vector<std::pair<std::string, std::string>> remaining;
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    if (consumed_[index]) {
      continue;
    }
    consumed_[index] = true;
    remaining.push_back(fields_[index]);
  }
  return remaining;
}

Status TextReader::finish() const {
  if (consumed_.size() != fields_.size()) {
    return Status::failure(ErrorCode::Internal, "text reader bookkeeping is inconsistent");
  }
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    if (!consumed_[index]) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "text payload contains an unexpected key: " + fields_[index].first);
    }
  }
  return Status{};
}

}  // namespace optical_fabric::detail
