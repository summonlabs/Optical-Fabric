// Optical Fabric 1.0.0 - Summon Software Labs
// Minimal dependency-free test harness.
//
// No watchdog and no test timeout is used: a hanging test is a defect and must
// surface as a hang. The only bounded waits are the ones used to observe a real
// operating-system child process, and exceeding such a bound produces an
// explicit failed assertion with a diagnostic - never a silent pass.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace of_test {

class Failure : public std::runtime_error {
 public:
  explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void fail(const char* file, int line, const std::string& expression, const std::string& detail);

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Runs every registered case, optionally filtered by a substring of
/// "suite.name". Returns the number of failures. Running zero cases fails.
int run_all(const std::string& filter);

/// Unique-per-process temporary directory under the operating-system temporary
/// area, using operating-system entropy so concurrent processes never collide.
class TempDirectory {
 public:
  TempDirectory();
  ~TempDirectory();

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const;

 private:
  std::filesystem::path path_;
};

struct ChildProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
};

/// A real operating-system child process with captured standard output. The
/// destructor always terminates and reaps the child, so a failing assertion can
/// never leave an orphan behind.
class ChildProcess {
 public:
  explicit ChildProcess(const ChildProcessOptions& options);
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool running();
  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }

  /// Waits up to the budget for one complete line of standard output. Returns
  /// std::nullopt when the budget expires or the child exits first; callers
  /// must turn that into an explicit failure.
  [[nodiscard]] std::optional<std::string> try_read_line(std::uint32_t budget_milliseconds);

  /// Terminates the child with an unconditional operating-system termination
  /// path: no graceful shutdown and no cooperative signal.
  void terminate();

  /// Waits for natural exit and returns the exit code, or std::nullopt when the
  /// budget expires.
  [[nodiscard]] std::optional<int> try_wait(std::uint32_t budget_milliseconds);

 private:
  void close_handles() noexcept;

  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* stdout_read_ = nullptr;
  std::uint32_t process_id_ = 0;
  bool exited_ = false;
  int exit_code_ = 0;
  std::string pending_;
  bool eof_ = false;
};

/// Deterministic seeded generator for property and adversarial suites. The seed
/// is reported on every failure so a counterexample is reproducible.
class DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed)
      : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed),
        seed_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_ = 0;
};

}  // namespace of_test

#define OF_TEST(suite, name)                                                       \
  static void of_test_body_##suite##_##name();                                     \
  static const ::of_test::Registrar of_test_registrar_##suite##_##name(             \
      #suite, #name, of_test_body_##suite##_##name);                                \
  static void of_test_body_##suite##_##name()

#define OF_REQUIRE(expression)                                                     \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::of_test::fail(__FILE__, __LINE__, #expression, "");                        \
    }                                                                              \
  } while (false)

#define OF_REQUIRE_MSG(expression, detail)                                         \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::of_test::fail(__FILE__, __LINE__, #expression, (detail));                  \
    }                                                                              \
  } while (false)

#define OF_REQUIRE_EQ(lhs, rhs)                                                    \
  do {                                                                             \
    const auto& of_lhs = (lhs);                                                    \
    const auto& of_rhs = (rhs);                                                    \
    if (!(of_lhs == of_rhs)) {                                                     \
      ::of_test::fail(__FILE__, __LINE__, #lhs " == " #rhs, "");                   \
    }                                                                              \
  } while (false)

#define OF_REQUIRE_NE(lhs, rhs)                                                    \
  do {                                                                             \
    const auto& of_lhs = (lhs);                                                    \
    const auto& of_rhs = (rhs);                                                    \
    if (of_lhs == of_rhs) {                                                        \
      ::of_test::fail(__FILE__, __LINE__, #lhs " != " #rhs, "");                   \
    }                                                                              \
  } while (false)

#define OF_REQUIRE_THROWS(expression)                                              \
  do {                                                                             \
    bool of_threw = false;                                                         \
    try {                                                                          \
      (void)(expression);                                                          \
    } catch (const std::exception&) {                                              \
      of_threw = true;                                                             \
    }                                                                              \
    if (!of_threw) {                                                               \
      ::of_test::fail(__FILE__, __LINE__, #expression " throws", "no exception");  \
    }                                                                              \
  } while (false)

#define OF_REQUIRE_NO_THROW(expression)                                            \
  do {                                                                             \
    try {                                                                          \
      (void)(expression);                                                          \
    } catch (const std::exception& of_error) {                                     \
      ::of_test::fail(__FILE__, __LINE__, #expression " does not throw",           \
                      of_error.what());                                            \
    }                                                                              \
  } while (false)
