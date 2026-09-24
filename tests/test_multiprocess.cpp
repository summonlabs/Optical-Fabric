// Optical Fabric 1.0.0 - Summon Software Labs
// Real multi-process proof: independent operating-system processes, a real
// loopback TCP control plane, and kills at distinct lifecycle boundaries.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "optical_fabric/client.hpp"
#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::ChildProcess;
using of_test::ChildProcessOptions;
using of_test::TempDirectory;

namespace {

constexpr std::uint32_t kAnnounceBudgetMs = 15000;
constexpr std::uint32_t kExitBudgetMs = 15000;

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string announce_field(const std::string& line, const std::string& key) {
  const std::string needle = key + "=";
  const std::size_t at = line.find(needle);
  OF_REQUIRE_MSG(at != std::string::npos, "the announcement line has no " + key);
  const std::size_t start = at + needle.size();
  const std::size_t end = line.find(' ', start);
  return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/// One independently running control-plane process.
struct NodeProcess {
  std::unique_ptr<ChildProcess> process;
  std::uint16_t port = 0;
  std::uint64_t boot_sequence = 0;
  std::string instance;
  std::unique_ptr<of::FabricClient> client;

  NodeProcess(const std::string& store_path, const std::vector<std::string>& extra = {}) {
    ChildProcessOptions options;
    options.executable = std::filesystem::path(OF_NODE_EXE);
    options.arguments = {"--store", store_path, "--port", "0", "--label", "multiprocess"};
    for (const std::string& argument : extra) {
      options.arguments.push_back(argument);
    }
    process = std::make_unique<ChildProcess>(options);
    const std::optional<std::string> line = process->try_read_line(kAnnounceBudgetMs);
    OF_REQUIRE_MSG(line.has_value(), "the node process produced no announcement line");
    port = static_cast<std::uint16_t>(std::stoi(announce_field(*line, "port")));
    boot_sequence = std::stoull(announce_field(*line, "boot_sequence"));
    instance = announce_field(*line, "instance");
    of::ClientOptions client_options;
    client_options.host = "127.0.0.1";
    client_options.port = port;
    client = std::make_unique<of::FabricClient>(client_options);
    const of::Status connected = client->connect();
    OF_REQUIRE_MSG(connected.ok(), "cannot connect to the control plane: " + connected.message);
  }

  of::ProtocolMessage call(const std::string& operation, Fields fields = {}) {
    of::ProtocolMessage request;
    request.operation = operation;
    request.fields = std::move(fields);
    const of::Result<of::ProtocolMessage> response = client->call(request);
    OF_REQUIRE_MSG(response.ok(), "control-plane call failed: " + response.status.message);
    return response.value;
  }

  of::ProtocolMessage require_ok(const std::string& operation, Fields fields = {}) {
    of::ProtocolMessage response = call(operation, std::move(fields));
    OF_REQUIRE_MSG(response.get("status") == "ok", operation + " refused: " + response.get("refusal") +
                                                    " " + response.get("detail"));
    return response;
  }

  void kill() {
    client.reset();
    process->terminate();
    (void)process->try_wait(kExitBudgetMs);
  }
};

/// One protocol field. A pair, so a braced list of fields converts directly.
std::pair<std::string, std::string> kv(const std::string& name, const std::string& value) {
  return std::make_pair(name, value);
}

struct RemoteLine {
  std::string site;
  std::string node_a;
  std::string node_b;
  std::string client_a;
  std::string client_b;
  std::string port_a;
  std::string port_b;
  std::string span;
  std::string cross_a;
  std::string cross_b;
  std::string channel;
};

std::string register_one(NodeProcess& node, const std::string& operation, Fields fields) {
  const of::ProtocolMessage response = node.require_ok(operation, std::move(fields));
  return response.get("resource");
}

RemoteLine register_line(NodeProcess& node, const std::string& prefix) {
  RemoteLine line;
  line.site = register_one(node, "register_site", {kv("name", prefix + ".site")});
  line.node_a = register_one(node, "register_node", {kv("name", prefix + ".node-a"), kv("site", line.site)});
  line.node_b = register_one(node, "register_node", {kv("name", prefix + ".node-b"), kv("site", line.site)});
  line.client_a = register_one(node, "register_port", {kv("name", prefix + ".client-a"), kv("node", line.node_a)});
  line.client_b = register_one(node, "register_port", {kv("name", prefix + ".client-b"), kv("node", line.node_b)});
  line.port_a = register_one(node, "register_port", {kv("name", prefix + ".port-a"), kv("node", line.node_a)});
  line.port_b = register_one(node, "register_port", {kv("name", prefix + ".port-b"), kv("node", line.node_b)});
  line.span = register_one(node, "register_span", {kv("name", prefix + ".span"), kv("endpoint_a", line.port_a),
                            kv("endpoint_b", line.port_b), kv("length", "10000")});
  line.cross_a = register_one(node, "register_cross_connect", {kv("name", prefix + ".cross-a"), kv("node", line.node_a),
                               kv("ingress", line.client_a), kv("egress", line.port_a)});
  line.cross_b = register_one(node, "register_cross_connect", {kv("name", prefix + ".cross-b"), kv("node", line.node_b),
                               kv("ingress", line.port_b), kv("egress", line.client_b)});
  line.channel = register_one(node, "register_channel", {kv("name", prefix + ".channel-0"), kv("index", "0"),
                               kv("frequency", "191300")});
  return line;
}

std::vector<std::string> path_resources(const RemoteLine& line) {
  return {line.client_a, line.cross_a, line.port_a, line.span,
          line.port_b,   line.cross_b, line.client_b, line.channel};
}

std::vector<std::string> requirement_kinds() {
  return {"port-capability",
          "span-capability",
          "cross-connect-capability",
          "channel-capability",
          "port-operational-state",
          "span-operational-state",
          "cross-connect-operational-state",
          "channel-availability",
          "wavelength-availability"};
}

void ingest_evidence(NodeProcess& node, const RemoteLine& line, std::uint64_t sequence_base,
                     std::uint64_t validity) {
  std::uint64_t sequence = sequence_base;
  const std::string source =
      of::derive_named_id<of::SourceId>("source", "multiprocess-synthetic").to_string();
  for (const std::string& subject : path_resources(line)) {
    for (const std::string& kind : requirement_kinds()) {
      node.require_ok("ingest_observation",
                      {kv("subject", subject), kv("kind", kind), kv("state", "known"),
                       kv("runtime", "multiprocess-synthetic"), kv("instance", "client"),
                       kv("source", source), kv("sequence", std::to_string(++sequence)),
                       kv("validity", std::to_string(validity)),
                       kv("detail", "synthetic observation over the control plane")});
    }
  }
}

struct RemoteAuthority {
  std::string grant;
  std::string holder;
  std::string epoch;
  std::string scope_kind;
  std::string scope_site;
  std::string incarnation_boot;
};

RemoteAuthority acquire(NodeProcess& node, const std::string& holder, const std::string& site) {
  const of::ProtocolMessage response =
      node.require_ok("acquire_authority",
                      {kv("holder", of::derive_named_id<of::ControllerId>("controller", holder).to_string()),
                       kv("scope_kind", "site"), kv("scope_site", site), kv("lease", "1000000"),
                       kv("attempt", of::generate_attempt_id().to_string())});
  RemoteAuthority authority;
  authority.grant = response.get("grant");
  authority.holder = response.get("holder");
  authority.epoch = response.get("epoch");
  authority.scope_kind = response.get("scope_kind");
  authority.scope_site = response.get("scope_site");
  authority.incarnation_boot = response.get("incarnation_boot");
  return authority;
}

Fields authority_fields(const RemoteAuthority& authority) {
  return {kv("grant", authority.grant),
          kv("holder", authority.holder),
          kv("epoch", authority.epoch),
          kv("scope_kind", authority.scope_kind),
          kv("scope_site", authority.scope_site),
          kv("incarnation_boot", authority.incarnation_boot)};
}

Fields with_authority(Fields base, const RemoteAuthority& authority) {
  Fields fields = authority_fields(authority);
  for (auto& entry : fields) {
    base.push_back(std::move(entry));
  }
  return base;
}

std::string submit(NodeProcess& node, const RemoteLine& line, const std::string& name) {
  const of::ProtocolMessage response =
      node.require_ok("submit_intent",
                      {kv("name", name), kv("owner", "multiprocess"), kv("source", line.client_a),
                       kv("destination", line.client_b), kv("channel", line.channel),
                       kv("ttl", "100000")});
  OF_REQUIRE_EQ(response.get("outcome"), std::string("accepted"));
  return response.get("connectivity");
}

std::string activate(NodeProcess& node, const RemoteLine& line, const RemoteAuthority& authority,
                     const std::string& name) {
  const std::string connectivity = submit(node, line, name);
  node.require_ok("validate",
                  with_authority({kv("connectivity", connectivity),
                                  kv("attempt", of::generate_attempt_id().to_string())},
                                 authority));
  node.require_ok("reserve",
                  with_authority({kv("connectivity", connectivity), kv("ttl", "100000"),
                                  kv("attempt", of::generate_attempt_id().to_string())},
                                 authority));
  const of::ProtocolMessage began = node.require_ok(
      "begin_activation",
      with_authority({kv("connectivity", connectivity),
                      kv("attempt", of::generate_attempt_id().to_string())},
                     authority));
  const of::ProtocolMessage committed = node.require_ok(
      "commit_activation",
      with_authority({kv("connectivity", connectivity),
                      kv("activation_digest", began.get("activation_digest")),
                      kv("attempt", of::generate_attempt_id().to_string())},
                     authority));
  OF_REQUIRE_EQ(committed.get("state"), std::string("active"));
  return connectivity;
}

}  // namespace

OF_TEST(multiprocess, restart_fences_the_previous_controller_and_requires_revalidation) {
  TempDirectory directory;
  const std::string store = directory.file("fabric.store").string();
  std::string connectivity;
  std::string site;
  std::string stale_incarnation;
  {
    NodeProcess first(store);
    OF_REQUIRE_EQ(first.boot_sequence, 1u);
    const RemoteLine line = register_line(first, "mp");
    site = line.site;
    const RemoteAuthority authority = acquire(first, "controller-a", line.site);
    stale_incarnation = authority.incarnation_boot;
    ingest_evidence(first, line, 0, 1000000);
    connectivity = activate(first, line, authority, "mp-path");
    const of::ProtocolMessage paths = first.require_ok("active_paths");
    OF_REQUIRE_EQ(paths.get("active"), std::string("1"));
    OF_REQUIRE_EQ(paths.get("authorized"), std::string("1"));
    const of::ProtocolMessage snapshot = first.require_ok("snapshot");
    OF_REQUIRE_EQ(snapshot.get("active"), std::string("1"));
    first.kill();
  }
  {
    NodeProcess second(store);
    // A restart is a new incarnation: the boot sequence advanced.
    OF_REQUIRE_EQ(second.boot_sequence, 2u);
    OF_REQUIRE_NE(second.instance, std::string());
    const of::ProtocolMessage hello = second.require_ok("hello");
    OF_REQUIRE_EQ(hello.get("boot_sequence"), std::string("2"));
    const of::ProtocolMessage paths = second.require_ok("active_paths");
    // The path survived as committed state, but nothing is authorized until
    // the new incarnation reconfirms authority and evidence.
    OF_REQUIRE_EQ(paths.get("active"), std::string("1"));
    OF_REQUIRE_EQ(paths.get("authorized"), std::string("0"));
    OF_REQUIRE_EQ(paths.get("entry.0.authorized"), std::string("0"));
    const of::ProtocolMessage accounting = second.require_ok("accounting");
    OF_REQUIRE_EQ(accounting.get("claims"), std::string("7"));
    const of::ProtocolMessage invariants = second.require_ok("verify_invariants");
    OF_REQUIRE_EQ(invariants.get("all_hold"), std::string("1"));

    // The token minted by the previous incarnation is refused: it names a dead
    // incarnation and a fenced epoch.
    RemoteAuthority stale;
    stale.grant = "0000000000000000";
    stale.holder = of::derive_named_id<of::ControllerId>("controller", "controller-a").to_string();
    stale.epoch = "1";
    stale.scope_kind = "site";
    stale.scope_site = site;
    stale.incarnation_boot = stale_incarnation;
    const of::ProtocolMessage refused = second.call(
        "withdraw",
        with_authority({kv("connectivity", connectivity), kv("reason", "stale controller"),
                        kv("attempt", of::generate_attempt_id().to_string())},
                       stale));
    OF_REQUIRE_EQ(refused.get("status"), std::string("refused"));
    OF_REQUIRE(refused.get("refusal") == "not_authoritative" || refused.get("refusal") == "stale_epoch" ||
               refused.get("refusal") == "stale_incarnation");
    OF_REQUIRE_EQ(second.require_ok("active_paths").get("authorized"), std::string("0"));

    // The new controller takes the scope, re-attests the evidence and
    // reconfirms the path.
    const RemoteAuthority fresh = acquire(second, "controller-b", site);
    const of::ProtocolMessage revalidation =
        second.call("revalidate",
                    with_authority({kv("connectivity", connectivity), kv("withdraw_on_failure", "0"),
                                    kv("attempt", of::generate_attempt_id().to_string())},
                                   fresh));
    OF_REQUIRE_EQ(revalidation.get("status"), std::string("refused"));
    OF_REQUIRE_EQ(revalidation.get("refusal"), std::string("stale_evidence"));

    const RemoteLine line = register_line(second, "mp");
    ingest_evidence(second, line, 1000, 1000000);
    second.require_ok("revalidate",
                      with_authority({kv("connectivity", connectivity),
                                      kv("attempt", of::generate_attempt_id().to_string())},
                                     fresh));
    OF_REQUIRE_EQ(second.require_ok("active_paths").get("authorized"), std::string("1"));
    OF_REQUIRE_EQ(second.require_ok("verify_invariants").get("all_hold"), std::string("1"));
    OF_REQUIRE_EQ(second.require_ok("accounting").get("balanced"), std::string("1"));
    second.kill();
  }
}

OF_TEST(multiprocess, a_kill_at_the_activation_boundary_leaves_nothing_active) {
  TempDirectory directory;
  const std::string store = directory.file("fabric.store").string();
  {
    NodeProcess first(store);
    const RemoteLine line = register_line(first, "mpb");
    const RemoteAuthority authority = acquire(first, "controller-a", line.site);
    ingest_evidence(first, line, 0, 1000000);
    const std::string connectivity = submit(first, line, "activation-boundary");
    first.require_ok("validate",
                     with_authority({kv("connectivity", connectivity),
                                     kv("attempt", of::generate_attempt_id().to_string())},
                                    authority));
    first.require_ok("reserve",
                     with_authority({kv("connectivity", connectivity), kv("ttl", "100000"),
                                     kv("attempt", of::generate_attempt_id().to_string())},
                                    authority));
    const of::ProtocolMessage began = first.require_ok(
        "begin_activation",
        with_authority({kv("connectivity", connectivity),
                        kv("attempt", of::generate_attempt_id().to_string())},
                       authority));
    OF_REQUIRE_EQ(began.get("state"), std::string("activating"));
    // Killed between the prepare and the commit: no acknowledgement was sent.
    first.kill();
  }
  {
    NodeProcess second(store);
    OF_REQUIRE_EQ(second.boot_sequence, 2u);
    const of::ProtocolMessage paths = second.require_ok("active_paths");
    OF_REQUIRE_EQ(paths.get("active"), std::string("0"));
    OF_REQUIRE_EQ(paths.get("authorized"), std::string("0"));
    OF_REQUIRE_EQ(second.require_ok("accounting").get("claims"), std::string("0"));
    const of::ProtocolMessage invariants = second.require_ok("verify_invariants");
    OF_REQUIRE_EQ(invariants.get("all_hold"), std::string("1"));
    second.kill();
  }
}

OF_TEST(multiprocess, two_processes_cannot_share_one_store) {
  TempDirectory directory;
  const std::string store = directory.file("fabric.store").string();
  NodeProcess first(store);
  OF_REQUIRE_EQ(first.boot_sequence, 1u);
  OF_REQUIRE_EQ(first.require_ok("hello").get("status"), std::string("ok"));

  // A second process on the same store must refuse to start rather than
  // become a second writer.
  ChildProcessOptions options;
  options.executable = std::filesystem::path(OF_NODE_EXE);
  options.arguments = {"--store", store, "--port", "0"};
  ChildProcess contender(options);
  const std::optional<std::string> line = contender.try_read_line(kAnnounceBudgetMs);
  if (line.has_value()) {
    // It announced, which means it started: it must then have been refused by
    // the store, which it prints and reports with a non-zero exit.
    const std::optional<int> code = contender.try_wait(kExitBudgetMs);
    OF_REQUIRE_MSG(code.has_value(), "the contender neither exited nor was refused");
    OF_REQUIRE_NE(*code, 0);
  } else {
    const std::optional<int> code = contender.try_wait(kExitBudgetMs);
    OF_REQUIRE_MSG(code.has_value(), "the contender never exited");
    OF_REQUIRE_NE(*code, 0);
  }
  contender.terminate();
  OF_REQUIRE_EQ(first.require_ok("hello").get("status"), std::string("ok"));
  first.kill();

  // With the first process gone the store is usable again.
  NodeProcess third(store);
  OF_REQUIRE_EQ(third.boot_sequence, 2u);
  const of::ProtocolMessage invariants = third.require_ok("verify_invariants");
  std::string failures;
  for (std::size_t index = 0; index < invariants.fields.size(); ++index) {
    const std::string prefix = "check." + std::to_string(index) + ".";
    if (invariants.get(prefix + "holds") == "0") {
      failures.append(invariants.get(prefix + "name")).append(": ").append(invariants.get(prefix + "detail")).append("; ");
    }
  }
  OF_REQUIRE_MSG(invariants.get("all_hold") == "1", failures);
  third.kill();
}

OF_TEST(multiprocess, a_recovered_path_keeps_its_claims_against_a_conflicting_request) {
  TempDirectory directory;
  const std::string store = directory.file("fabric.store").string();
  std::string site;
  {
    NodeProcess first(store);
    const RemoteLine line = register_line(first, "mpc");
    site = line.site;
    const RemoteAuthority authority = acquire(first, "controller-a", line.site);
    ingest_evidence(first, line, 0, 1000000);
    activate(first, line, authority, "mp-conflict");
    first.kill();
  }
  {
    NodeProcess second(store);
    const RemoteLine line = register_line(second, "mpc");
    OF_REQUIRE_EQ(line.site, site);
    const RemoteAuthority authority = acquire(second, "controller-b", site);
    ingest_evidence(second, line, 500, 1000000);
    // The reverse orientation is a genuinely different canonical path that
    // needs the same exclusive resources, and the recovered path still holds
    // them.
    const of::ProtocolMessage submitted =
        second.require_ok("submit_intent",
                          {kv("name", "mp-reversed"), kv("source", line.client_b),
                           kv("destination", line.client_a), kv("channel", line.channel),
                           kv("direction", "reverse"), kv("ttl", "100000")});
    OF_REQUIRE_EQ(submitted.get("outcome"), std::string("accepted"));
    const std::string connectivity = submitted.get("connectivity");
    second.require_ok("validate",
                      with_authority({kv("connectivity", connectivity),
                                      kv("attempt", of::generate_attempt_id().to_string())},
                                     authority));
    const of::ProtocolMessage refused =
        second.call("reserve",
                    with_authority({kv("connectivity", connectivity), kv("ttl", "100000"),
                                    kv("attempt", of::generate_attempt_id().to_string())},
                                   authority));
    OF_REQUIRE_EQ(refused.get("status"), std::string("refused"));
    OF_REQUIRE_EQ(refused.get("refusal"), std::string("conflicting_claim"));
    OF_REQUIRE_EQ(second.require_ok("active_paths").get("active"), std::string("1"));
    OF_REQUIRE_EQ(second.require_ok("verify_invariants").get("all_hold"), std::string("1"));
    second.kill();
  }
}
