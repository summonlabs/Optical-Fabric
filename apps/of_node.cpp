// Optical Fabric 1.0.0 - Summon Software Labs
// Control-plane node: one runtime, one durable store, one loopback endpoint.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "optical_fabric/optical_fabric.hpp"
#include "optical_fabric/server.hpp"
#include "optical_fabric/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace of = optical_fabric;

namespace {

of::FabricNode* g_node = nullptr;

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    if (g_node != nullptr) {
      g_node->stop();
    }
    return TRUE;
  }
  return FALSE;
}
#endif

void usage() {
  std::fprintf(stderr,
               "usage: of_node --store <path> [--port <n>] [--host <address>] [--label <name>] "
               "[--salvage discard-tail|reject] [--no-fsync]\n");
}

}  // namespace

int main(int argc, char** argv) {
  of::NodeOptions options;
  options.limits.max_connections = 8;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&](const char* name) -> std::string {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "of_node: %s needs a value\n", name);
        std::exit(2);
      }
      return argv[++index];
    };
    if (argument == "--store") {
      options.fabric.store_path = next("--store");
    } else if (argument == "--port") {
      options.port = static_cast<std::uint16_t>(std::stoi(next("--port")));
    } else if (argument == "--host") {
      options.bind_host = next("--host");
    } else if (argument == "--label") {
      options.fabric.host_label = next("--label");
    } else if (argument == "--salvage") {
      const std::string mode = next("--salvage");
      if (mode == "reject") {
        options.fabric.salvage = of::SalvagePolicy::Reject;
      } else if (mode == "discard-tail") {
        options.fabric.salvage = of::SalvagePolicy::DiscardTail;
      } else {
        std::fprintf(stderr, "of_node: unknown salvage policy %s\n", mode.c_str());
        return 2;
      }
    } else if (argument == "--no-fsync") {
      options.fabric.durable_writes = false;
    } else if (argument == "--version") {
      std::printf("%s\n", std::string(of::version_string()).c_str());
      return 0;
    } else if (argument == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "of_node: unknown argument %s\n", argument.c_str());
      usage();
      return 2;
    }
  }
  if (options.fabric.store_path.empty()) {
    std::fprintf(stderr, "of_node: --store is required\n");
    usage();
    return 2;
  }

  of::FabricNode node(options);
  g_node = &node;
#if defined(_WIN32)
  SetConsoleCtrlHandler(console_handler, TRUE);
#endif
  const of::Status started = node.start();
  if (!started.ok()) {
    std::fprintf(stderr, "of_node: cannot start: %s\n", started.message.c_str());
    return 1;
  }
  if (node.fabric().closed()) {
    const of::RecoveryReport report = node.fabric().recovery();
    std::fprintf(stderr, "of_node: the store was refused: %s\n", report.detail.c_str());
    return 1;
  }
  const of::Status served = node.serve();
  node.stop();
  g_node = nullptr;
  if (!served.ok()) {
    std::fprintf(stderr, "of_node: %s\n", served.message.c_str());
    return 1;
  }
  return 0;
}
