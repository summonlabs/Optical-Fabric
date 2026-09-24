// Optical Fabric 1.0.0 - Summon Software Labs
#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "optical_fabric/digest.hpp"

namespace of_test {

namespace {

std::string hex_suffix() {
  std::uint8_t entropy[8] = {};
  optical_fabric::fill_random_bytes(entropy, sizeof(entropy));
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t value : entropy) {
    out.push_back(kDigits[(value >> 4) & 0x0Fu]);
    out.push_back(kDigits[value & 0x0Fu]);
  }
  return out;
}

[[nodiscard]] std::uint64_t now_milliseconds() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

void fail(const char* file, int line, const std::string& expression, const std::string& detail) {
  std::string message = std::string(file) + ":" + std::to_string(line) + ": requirement failed: " + expression;
  if (!detail.empty()) {
    message.append(" -- ").append(detail);
  }
  throw Failure(message);
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

int run_all(const std::string& filter) {
  std::vector<TestCase>& cases = registry();
  std::stable_sort(cases.begin(), cases.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });
  std::size_t executed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : cases) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    executed += 1;
    try {
      test.body();
      std::printf("[ pass ] %s\n", full.c_str());
    } catch (const Failure& failure) {
      failed += 1;
      std::printf("[ FAIL ] %s\n         %s\n", full.c_str(), failure.what());
    } catch (const std::exception& error) {
      failed += 1;
      std::printf("[ FAIL ] %s\n         unexpected exception: %s\n", full.c_str(), error.what());
    }
    std::fflush(stdout);
  }
  std::printf("---\n%d case(s) executed, %d failure(s)\n", static_cast<int>(executed),
              static_cast<int>(failed));
  if (executed == 0) {
    std::printf("no test case matched the filter [%s]\n", filter.c_str());
    return 1;
  }
  return failed == 0 ? 0 : 1;
}

TempDirectory::TempDirectory() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    throw Failure("cannot resolve the operating-system temporary directory");
  }
  path_ = base / ("optical_fabric_test_" + hex_suffix());
  std::filesystem::create_directories(path_, error);
  if (error) {
    throw Failure("cannot create the test workspace");
  }
}

TempDirectory::~TempDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDirectory::file(const std::string& name) const { return path_ / name; }

#if defined(_WIN32)

ChildProcess::ChildProcess(const ChildProcessOptions& options) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!CreatePipe(&read_handle, &write_handle, &attributes, 0)) {
    throw Failure("cannot create the child stdout pipe");
  }
  SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

  std::string command_line = "\"" + options.executable.string() + "\"";
  for (const std::string& argument : options.arguments) {
    command_line.append(" \"").append(argument).append("\"");
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_handle;
  startup.hStdError = write_handle;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION info{};
  const std::string working = options.working_directory.empty()
                                  ? std::string()
                                  : options.working_directory.string();
  const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr,
                                      working.empty() ? nullptr : working.c_str(), &startup, &info);
  CloseHandle(write_handle);
  if (!created) {
    CloseHandle(read_handle);
    throw Failure("cannot start the child process: " + options.executable.string());
  }
  CloseHandle(info.hThread);
  process_ = info.hProcess;
  stdout_read_ = read_handle;
  process_id_ = static_cast<std::uint32_t>(info.dwProcessId);
}

void ChildProcess::close_handles() noexcept {
  if (stdout_read_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (process_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
}

bool ChildProcess::running() {
  if (process_ == nullptr) {
    return false;
  }
  if (exited_) {
    return false;
  }
  const DWORD status = WaitForSingleObject(static_cast<HANDLE>(process_), 0);
  if (status == WAIT_TIMEOUT) {
    return true;
  }
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return false;
}

std::optional<std::string> ChildProcess::try_read_line(std::uint32_t budget_milliseconds) {
  const std::uint64_t deadline = now_milliseconds() + budget_milliseconds;
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      while (!line.empty() && (line.back() == '\r')) {
        line.pop_back();
      }
      return line;
    }
    if (eof_) {
      return std::nullopt;
    }
    if (now_milliseconds() >= deadline) {
      return std::nullopt;
    }
    DWORD available = 0;
    if (stdout_read_ == nullptr ||
        !PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &available, nullptr)) {
      eof_ = true;
      continue;
    }
    if (available == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    char buffer[4096];
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(stdout_read_), buffer, sizeof(buffer), &read, nullptr) || read == 0) {
      eof_ = true;
      continue;
    }
    pending_.append(buffer, read);
  }
}

void ChildProcess::terminate() {
  if (process_ == nullptr) {
    return;
  }
  if (!exited_) {
    TerminateProcess(static_cast<HANDLE>(process_), 137);
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
  }
}

std::optional<int> ChildProcess::try_wait(std::uint32_t budget_milliseconds) {
  if (process_ == nullptr) {
    return exit_code_;
  }
  if (exited_) {
    return exit_code_;
  }
  const DWORD status = WaitForSingleObject(static_cast<HANDLE>(process_), budget_milliseconds);
  if (status == WAIT_TIMEOUT) {
    return std::nullopt;
  }
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
  exited_ = true;
  return exit_code_;
}

ChildProcess::~ChildProcess() {
  terminate();
  close_handles();
}

#else

ChildProcess::ChildProcess(const ChildProcessOptions& options) {
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw Failure("cannot create the child stdout pipe");
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    throw Failure("cannot fork the child process");
  }
  if (pid == 0) {
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(options.executable.c_str()));
    for (const std::string& argument : options.arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    if (!options.working_directory.empty()) {
      (void)chdir(options.working_directory.c_str());
    }
    execv(options.executable.c_str(), argv.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pipe_fds[0]));
  process_id_ = static_cast<std::uint32_t>(pid);
}

void ChildProcess::close_handles() noexcept {
  if (stdout_read_ != nullptr) {
    close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
    stdout_read_ = nullptr;
  }
}

bool ChildProcess::running() {
  if (process_ == nullptr || exited_) {
    return false;
  }
  int status = 0;
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  const pid_t result = waitpid(pid, &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 137;
  exited_ = true;
  return false;
}

std::optional<std::string> ChildProcess::try_read_line(std::uint32_t budget_milliseconds) {
  const std::uint64_t deadline = now_milliseconds() + budget_milliseconds;
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_));
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      while (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    if (eof_ || now_milliseconds() >= deadline) {
      return std::nullopt;
    }
    char buffer[4096];
    const ssize_t read = ::read(fd, buffer, sizeof(buffer));
    if (read <= 0) {
      eof_ = true;
      continue;
    }
    pending_.append(buffer, static_cast<std::size_t>(read));
  }
}

void ChildProcess::terminate() {
  if (process_ == nullptr || exited_) {
    return;
  }
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  exit_code_ = 137;
  exited_ = true;
}

std::optional<int> ChildProcess::try_wait(std::uint32_t budget_milliseconds) {
  if (process_ == nullptr) {
    return exit_code_;
  }
  if (exited_) {
    return exit_code_;
  }
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  const std::uint64_t deadline = now_milliseconds() + budget_milliseconds;
  while (now_milliseconds() < deadline) {
    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 137;
      exited_ = true;
      return exit_code_;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return std::nullopt;
}

ChildProcess::~ChildProcess() {
  terminate();
  close_handles();
}

#endif

}  // namespace of_test

int main(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--filter" && index + 1 < argc) {
      filter = argv[++index];
    } else if (filter.empty()) {
      filter = argument;
    }
  }
  return of_test::run_all(filter);
}
