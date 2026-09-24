// Optical Fabric 1.0.0 - Summon Software Labs
#include "store.hpp"

#include <array>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "crc_stream.hpp"
#include "optical_fabric/digest.hpp"
#include "optical_fabric/version.hpp"

namespace optical_fabric {

namespace {

void write_u16(std::string& out, std::uint16_t value);

}  // namespace

/// The record checksum covers the frame header and the payload as one stream,
/// so the writer and the reader cannot disagree about the algorithm.
std::uint32_t frame_crc(std::uint16_t kind, std::uint32_t payload_length, const std::string& payload) noexcept {
  std::string header;
  header.reserve(8);
  {
    header.push_back(static_cast<char>(kind & 0xFFu));
    header.push_back(static_cast<char>((kind >> 8) & 0xFFu));
    header.push_back(static_cast<char>(0));
    header.push_back(static_cast<char>(0));
    for (int index = 0; index < 4; ++index) {
      header.push_back(static_cast<char>((payload_length >> (8 * index)) & 0xFFu));
    }
  }
  detail::Crc32cStream stream;
  stream.update(header.data(), header.size());
  if (!payload.empty()) {
    stream.update(payload.data(), payload.size());
  }
  return stream.finish();
}

}  // namespace optical_fabric

namespace optical_fabric::detail {

namespace {

constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kRecordHeaderBytes = 16;

void write_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void write_u32(std::string& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
  }
}

void write_u64(std::string& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
  }
}

[[nodiscard]] std::uint16_t read_u16(const char* data) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) |
                                    (static_cast<unsigned char>(data[1]) << 8));
}

[[nodiscard]] std::uint32_t read_u32(const char* data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << (8 * index);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(const char* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[index])) << (8 * index);
  }
  return value;
}

/// Header: magic u32 | format version u32 | flags u32 | crc u32 | boot u64.
/// The checksum covers exactly the first 12 bytes.
[[nodiscard]] std::string build_header(std::uint64_t created_boot) {
  std::string header;
  header.reserve(kHeaderBytes);
  write_u32(header, kStoreMagic);
  write_u32(header, kStateFormatVersion);
  write_u32(header, 0);
  write_u32(header, crc32c(header.data(), header.size()));
  write_u64(header, created_boot);
  return header;
}

[[nodiscard]] std::string build_record(RecordKind kind, const std::string& payload) {
  std::string frame;
  frame.reserve(kRecordHeaderBytes + payload.size());
  write_u32(frame, kRecordMagic);
  write_u16(frame, static_cast<std::uint16_t>(kind));
  write_u16(frame, 0);
  write_u32(frame, static_cast<std::uint32_t>(payload.size()));
  write_u32(frame, frame_crc(static_cast<std::uint16_t>(kind),
                             static_cast<std::uint32_t>(payload.size()), payload));
  frame.append(payload);
  return frame;
}

}  // namespace

struct Store::Impl {
  std::filesystem::path path;
  std::filesystem::path lock_path;
  SalvagePolicy salvage = SalvagePolicy::DiscardTail;
  bool exclusive_lock = true;
#if defined(_WIN32)
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE lock = INVALID_HANDLE_VALUE;
#else
  int file = -1;
  int lock = -1;
#endif
};

namespace {

void close_lock(Store::Impl& impl) {
#if defined(_WIN32)
  if (impl.lock != INVALID_HANDLE_VALUE) {
    CloseHandle(impl.lock);
    impl.lock = INVALID_HANDLE_VALUE;
  }
#else
  if (impl.lock >= 0) {
    ::close(impl.lock);
    impl.lock = -1;
  }
#endif
}

Status acquire_lock(Store::Impl& impl) {
  if (!impl.exclusive_lock) {
    return Status{};
  }
  impl.lock_path = impl.path;
  impl.lock_path += ".lock";
#if defined(_WIN32)
  const std::wstring wide = impl.lock_path.wstring();
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::failure(ErrorCode::StoreLocked,
                           "another process holds the writer lock for this store");
  }
  impl.lock = handle;
#else
  const std::string native = impl.lock_path.string();
  const int handle = ::open(native.c_str(), O_CREAT | O_RDWR, 0644);
  if (handle < 0) {
    return Status::failure(ErrorCode::StoreIo, "cannot create the store lock file");
  }
  if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
    ::close(handle);
    return Status::failure(ErrorCode::StoreLocked,
                           "another process holds the writer lock for this store");
  }
  impl.lock = handle;
#endif
  return Status{};
}

Status open_file(Store::Impl& impl, bool for_write) {
#if defined(_WIN32)
  const std::wstring wide = impl.path.wstring();
  const DWORD access = for_write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  const DWORD share = for_write ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE);
  const DWORD disposition = for_write ? OPEN_ALWAYS : OPEN_EXISTING;
  HANDLE handle = CreateFileW(wide.c_str(), access, share, nullptr, disposition,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::failure(ErrorCode::StoreIo, "cannot open the store file");
  }
  impl.file = handle;
#else
  const std::string native = impl.path.string();
  const int flags = for_write ? (O_CREAT | O_RDWR) : O_RDONLY;
  const int handle = ::open(native.c_str(), flags, 0644);
  if (handle < 0) {
    return Status::failure(ErrorCode::StoreIo, "cannot open the store file");
  }
  impl.file = handle;
#endif
  return Status{};
}

Status read_range(Store::Impl& impl, std::uint64_t offset, std::size_t length, std::string& out) {
  out.assign(length, '\0');
  if (length == 0) {
    return Status{};
  }
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(impl.file, position, nullptr, FILE_BEGIN)) {
    return Status::failure(ErrorCode::StoreIo, "cannot seek in the store file");
  }
  DWORD total = 0;
  while (total < length) {
    DWORD chunk = 0;
    const DWORD want = static_cast<DWORD>(length - total);
    if (!ReadFile(impl.file, out.data() + total, want, &chunk, nullptr)) {
      return Status::failure(ErrorCode::StoreIo, "cannot read the store file");
    }
    if (chunk == 0) {
      return Status::failure(ErrorCode::StoreIo, "the store file ended before the declared length");
    }
    total += chunk;
  }
#else
  std::size_t total = 0;
  while (total < length) {
    const ssize_t chunk = ::pread(impl.file, out.data() + total, length - total,
                                  static_cast<off_t>(offset + total));
    if (chunk < 0) {
      return Status::failure(ErrorCode::StoreIo, "cannot read the store file");
    }
    if (chunk == 0) {
      return Status::failure(ErrorCode::StoreIo, "the store file ended before the declared length");
    }
    total += static_cast<std::size_t>(chunk);
  }
#endif
  return Status{};
}

Status write_all(Store::Impl& impl, const std::string& data) {
  if (data.empty()) {
    return Status{};
  }
#if defined(_WIN32)
  DWORD total = 0;
  while (total < data.size()) {
    DWORD written = 0;
    const DWORD want = static_cast<DWORD>(data.size() - total);
    if (!WriteFile(impl.file, data.data() + total, want, &written, nullptr)) {
      return Status::failure(ErrorCode::StoreIo, "cannot append to the store file");
    }
    if (written == 0) {
      return Status::failure(ErrorCode::StoreIo, "the store file accepted no bytes");
    }
    total += written;
  }
#else
  std::size_t total = 0;
  while (total < data.size()) {
    const ssize_t written = ::write(impl.file, data.data() + total, data.size() - total);
    if (written < 0) {
      return Status::failure(ErrorCode::StoreIo, "cannot append to the store file");
    }
    if (written == 0) {
      return Status::failure(ErrorCode::StoreIo, "the store file accepted no bytes");
    }
    total += static_cast<std::size_t>(written);
  }
#endif
  return Status{};
}

Status sync_file(Store::Impl& impl) {
#if defined(_WIN32)
  if (!FlushFileBuffers(impl.file)) {
    return Status::failure(ErrorCode::StoreIo, "cannot flush the store file");
  }
#else
  if (::fsync(impl.file) != 0) {
    return Status::failure(ErrorCode::StoreIo, "cannot flush the store file");
  }
#endif
  return Status{};
}

Status seek_to(Store::Impl& impl, std::uint64_t offset) {
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(impl.file, position, nullptr, FILE_BEGIN)) {
    return Status::failure(ErrorCode::StoreIo, "cannot seek in the store file");
  }
#else
  if (::lseek(impl.file, static_cast<off_t>(offset), SEEK_SET) < 0) {
    return Status::failure(ErrorCode::StoreIo, "cannot seek in the store file");
  }
#endif
  return Status{};
}

Status truncate_to(Store::Impl& impl, std::uint64_t length) {
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(length);
  if (!SetFilePointerEx(impl.file, position, nullptr, FILE_BEGIN)) {
    return Status::failure(ErrorCode::StoreIo, "cannot seek to truncate the store file");
  }
  if (!SetEndOfFile(impl.file)) {
    return Status::failure(ErrorCode::StoreIo, "cannot truncate the store file");
  }
#else
  if (::ftruncate(impl.file, static_cast<off_t>(length)) != 0) {
    return Status::failure(ErrorCode::StoreIo, "cannot truncate the store file");
  }
#endif
  // ftruncate does not move the file offset, and a write past the old offset
  // would silently punch a hole in the file. Always reposition explicitly.
  return seek_to(impl, length);
}

Result<std::uint64_t> file_size(Store::Impl& impl) {
#if defined(_WIN32)
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(impl.file, &size)) {
    return Result<std::uint64_t>::failure(ErrorCode::StoreIo, "cannot size the store file");
  }
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
#else
  struct stat info {};
  if (::fstat(impl.file, &info) != 0) {
    return Result<std::uint64_t>::failure(ErrorCode::StoreIo, "cannot size the store file");
  }
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
#endif
}

void close_file(Store::Impl& impl) {
#if defined(_WIN32)
  if (impl.file != INVALID_HANDLE_VALUE) {
    CloseHandle(impl.file);
    impl.file = INVALID_HANDLE_VALUE;
  }
#else
  if (impl.file >= 0) {
    ::close(impl.file);
    impl.file = -1;
  }
#endif
}

[[nodiscard]] bool looks_like_record_magic(const std::string& bytes, std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return false;
  }
  return read_u32(bytes.data() + offset) == kRecordMagic;
}

}  // namespace

Store::Store(std::filesystem::path path, SalvagePolicy salvage, bool durable_writes, bool exclusive_lock,
             Limits limits)
    : impl_(std::make_unique<Impl>()), durable_writes_(durable_writes), limits_(limits) {
  impl_->path = std::move(path);
  impl_->salvage = salvage;
  impl_->exclusive_lock = exclusive_lock;
  memory_only_ = impl_->path.empty();
}

Store::~Store() { close(); }

Status Store::open(RecoveryReport& report) {
  if (memory_only_) {
    report.status = RecoveryStatus::MemoryOnly;
    report.detail = "no store path was configured; state is in memory only";
    return Status{};
  }
#if defined(_WIN32)
  impl_->file = INVALID_HANDLE_VALUE;
  impl_->lock = INVALID_HANDLE_VALUE;
#else
  impl_->file = -1;
  impl_->lock = -1;
#endif

  std::error_code error;
  const bool existed = std::filesystem::exists(impl_->path, error);
  if (error) {
    report.status = RecoveryStatus::Rejected;
    report.detail = "cannot stat the store path";
    return Status::failure(ErrorCode::StoreIo, report.detail);
  }

  const Status locked = acquire_lock(*impl_);
  if (!locked.ok()) {
    report.status = RecoveryStatus::Rejected;
    report.detail = locked.message;
    return locked;
  }

  const Status opened = open_file(*impl_, true);
  if (!opened.ok()) {
    close_lock(*impl_);
    report.status = RecoveryStatus::Rejected;
    report.detail = opened.message;
    return opened;
  }
  open_ = true;

  // From here on, every failure path must release the lock and the handle so a
  // refused store does not block the next attempt.
  const auto fail = [&](ErrorCode code, const std::string& detail) {
    close_file(*impl_);
    close_lock(*impl_);
    open_ = false;
    report.status = RecoveryStatus::Rejected;
    report.detail = detail;
    return Status::failure(code, detail);
  };

  const Result<std::uint64_t> size = file_size(*impl_);
  if (!size.ok()) {
    return fail(size.status.code, size.status.message);
  }
  report.file_bytes = size.value;

  if (!existed || size.value == 0) {
    const std::string header = build_header(1);
    const Status written = write_all(*impl_, header);
    if (!written.ok()) {
      return fail(written.code, written.message);
    }
    const Status positioned = seek_to(*impl_, kHeaderBytes);
    if (!positioned.ok()) {
      return fail(positioned.code, positioned.message);
    }
    valid_bytes_ = kHeaderBytes;
    appended_bytes_ = 0;
    appended_records_ = 0;
    report.status = RecoveryStatus::Created;
    report.detail = "the store did not exist and was created";
    report.records_read = 0;
    if (durable_writes_) {
      const Status synced = sync_file(*impl_);
      if (!synced.ok()) {
        return fail(synced.code, synced.message);
      }
    }
    return Status{};
  }

  if (size.value < kHeaderBytes) {
    // A store that never received a complete header is a torn creation.
    if (impl_->salvage == SalvagePolicy::Reject) {
      return fail(ErrorCode::StoreCorrupt, "the store header is incomplete");
    }
    const Status truncated = truncate_to(*impl_, 0);
    if (!truncated.ok()) {
      return fail(truncated.code, truncated.message);
    }
    const std::string header = build_header(1);
    const Status written = write_all(*impl_, header);
    if (!written.ok()) {
      return fail(written.code, written.message);
    }
    const Status positioned = seek_to(*impl_, kHeaderBytes);
    if (!positioned.ok()) {
      return fail(positioned.code, positioned.message);
    }
    valid_bytes_ = kHeaderBytes;
    appended_bytes_ = 0;
    appended_records_ = 0;
    report.status = RecoveryStatus::TailDiscarded;
    report.discarded_bytes = size.value;
    report.detail = "an incomplete store header was replaced";
    return Status{};
  }

  std::string header;
  const Status header_read = read_range(*impl_, 0, kHeaderBytes, header);
  if (!header_read.ok()) {
    return fail(header_read.code, header_read.message);
  }
  if (read_u32(header.data()) != kStoreMagic) {
    return fail(ErrorCode::StoreCorrupt, "the store magic is wrong");
  }
  const std::uint32_t version = read_u32(header.data() + 4);
  if (version != kStateFormatVersion) {
    return fail(ErrorCode::StoreVersionUnsupported,
                "the store was written by format version " + std::to_string(version));
  }
  const std::uint32_t expected_header_crc = crc32c(header.data(), 12);
  if (read_u32(header.data() + 12) != expected_header_crc) {
    return fail(ErrorCode::StoreCorrupt, "the store header checksum does not match");
  }

  std::uint64_t offset = kHeaderBytes;
  std::uint64_t last_good = kHeaderBytes;
  bool tail_problem = false;
  std::string tail_detail;

  while (offset < size.value) {
    const std::uint64_t remaining = size.value - offset;
    if (remaining < kRecordHeaderBytes) {
      tail_problem = true;
      tail_detail = "the store ends with an incomplete record header";
      break;
    }
    std::string record_header;
    const Status read_status = read_range(*impl_, offset, kRecordHeaderBytes, record_header);
    if (!read_status.ok()) {
      return fail(read_status.code, read_status.message);
    }
    if (read_u32(record_header.data()) != kRecordMagic) {
      tail_problem = true;
      tail_detail = "a record frame does not begin with the record magic";
      break;
    }
    const auto kind = static_cast<RecordKind>(read_u16(record_header.data() + 4));
    const std::uint32_t payload_length = read_u32(record_header.data() + 8);
    const std::uint32_t stored_crc = read_u32(record_header.data() + 12);
    if (payload_length > limits_.max_record_payload_bytes) {
      // A length beyond the configured bound is never a torn tail: it is
      // corruption or a hostile file. Refuse rather than allocate from it.
      return fail(ErrorCode::StoreCorrupt,
                  "a record declares a payload larger than the configured bound");
    }
    if (remaining < kRecordHeaderBytes + payload_length) {
      tail_problem = true;
      tail_detail = "the store ends inside a record payload";
      break;
    }
    std::string payload;
    const Status payload_status = read_range(*impl_, offset + kRecordHeaderBytes, payload_length, payload);
    if (!payload_status.ok()) {
      return fail(payload_status.code, payload_status.message);
    }
    if (frame_crc(static_cast<std::uint16_t>(kind), payload_length, payload) != stored_crc) {
      // Distinguish a torn tail from mid-file corruption: if a further record
      // frame begins exactly where this one should have ended, the file
      // contains good data after the damage and must not be silently truncated.
      const std::uint64_t next_offset = offset + kRecordHeaderBytes + payload_length;
      if (next_offset < size.value) {
        std::string probe;
        const std::uint64_t probe_length = size.value - next_offset < 4 ? size.value - next_offset : 4;
        if (read_range(*impl_, next_offset, static_cast<std::size_t>(probe_length), probe).ok() &&
            looks_like_record_magic(probe, 0)) {
          return fail(ErrorCode::StoreCorrupt,
                      "a record checksum does not match and valid records follow it");
        }
      }
      tail_problem = true;
      tail_detail = "the last record does not match its checksum";
      break;
    }
    records_.push_back(StoreRecord{kind, std::move(payload)});
    offset += kRecordHeaderBytes + payload_length;
    last_good = offset;
  }

  report.records_read = records_.size();
  if (tail_problem) {
    if (impl_->salvage == SalvagePolicy::Reject) {
      return fail(ErrorCode::StoreCorrupt, tail_detail + " (salvage policy is reject)");
    }
    const Status truncated = truncate_to(*impl_, last_good);
    if (!truncated.ok()) {
      return fail(truncated.code, truncated.message);
    }
    report.status = RecoveryStatus::TailDiscarded;
    report.discarded_bytes = size.value - last_good;
    report.records_discarded = 1;
    report.detail = tail_detail + "; the trailing bytes were discarded";
  } else {
    report.status = RecoveryStatus::Clean;
    report.detail = "every record was intact";
  }
  const Status positioned = seek_to(*impl_, last_good);
  if (!positioned.ok()) {
    return fail(positioned.code, positioned.message);
  }
  valid_bytes_ = last_good;
  appended_bytes_ = 0;
  appended_records_ = 0;
  if (durable_writes_) {
    const Status synced = sync_file(*impl_);
    if (!synced.ok()) {
      return fail(synced.code, synced.message);
    }
  }
  return Status{};
}

Status Store::append(RecordKind kind, const std::string& payload) {
  if (memory_only_ || !open_) {
    return Status{};
  }
  if (payload.size() > limits_.max_record_payload_bytes) {
    return Status::failure(ErrorCode::CapacityExceeded, "a record exceeds the configured payload bound");
  }
  const std::string frame = build_record(kind, payload);
  const Status written = write_all(*impl_, frame);
  if (!written.ok()) {
    return written;
  }
  appended_bytes_ += frame.size();
  appended_records_ += 1;
  if (durable_writes_) {
    return sync_file(*impl_);
  }
  return Status{};
}

Status Store::rewrite(RecordKind kind, const std::string& payload) {
  if (memory_only_ || !open_) {
    return Status{};
  }
  if (payload.size() > limits_.max_record_payload_bytes) {
    return Status::failure(ErrorCode::CapacityExceeded, "a checkpoint exceeds the configured payload bound");
  }
  std::filesystem::path temporary = impl_->path;
  temporary += ".compact";
  const std::string frame = build_record(kind, payload);
  std::string contents = build_header(1);
  contents.append(frame);

  std::error_code error;
  std::filesystem::remove(temporary, error);
  error.clear();
  {
#if defined(_WIN32)
    const std::wstring wide = temporary.wstring();
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return Status::failure(ErrorCode::StoreIo, "cannot create the compaction file");
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written,
                              nullptr);
    if (ok && written == contents.size()) {
      FlushFileBuffers(handle);
    }
    CloseHandle(handle);
    if (!ok || written != contents.size()) {
      return Status::failure(ErrorCode::StoreIo, "cannot write the compaction file");
    }
#else
    const std::string native = temporary.string();
    const int handle = ::open(native.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (handle < 0) {
      return Status::failure(ErrorCode::StoreIo, "cannot create the compaction file");
    }
    std::size_t total = 0;
    while (total < contents.size()) {
      const ssize_t chunk = ::write(handle, contents.data() + total, contents.size() - total);
      if (chunk <= 0) {
        ::close(handle);
        return Status::failure(ErrorCode::StoreIo, "cannot write the compaction file");
      }
      total += static_cast<std::size_t>(chunk);
    }
    ::fsync(handle);
    ::close(handle);
#endif
  }

  // Close the live handle before replacing the file: the replacement reuses the
  // same path and must not race the handle that is being replaced.
  close_file(*impl_);
  std::filesystem::rename(temporary, impl_->path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return Status::failure(ErrorCode::StoreIo, "cannot replace the store with its compaction");
  }
  const Status reopened = open_file(*impl_, true);
  if (!reopened.ok()) {
    close_lock(*impl_);
    open_ = false;
    return reopened;
  }
  const Status positioned = seek_to(*impl_, contents.size());
  if (!positioned.ok()) {
    close_file(*impl_);
    close_lock(*impl_);
    open_ = false;
    return positioned;
  }
  records_.clear();
  records_.push_back(StoreRecord{kind, payload});
  valid_bytes_ = contents.size();
  appended_bytes_ = 0;
  appended_records_ = 0;
  if (durable_writes_) {
    return sync_file(*impl_);
  }
  return Status{};
}

Status Store::flush() {
  if (memory_only_ || !open_) {
    return Status{};
  }
  return sync_file(*impl_);
}

void Store::close() {
  if (memory_only_) {
    return;
  }
  if (open_) {
    close_file(*impl_);
  }
  close_lock(*impl_);
  open_ = false;
}

}  // namespace optical_fabric::detail
