// Optional live evidence for ck-exec CLIENT halves: the owner of a grid builds a mod from the
// mod starter and a CLIENT half from a crowdy-client-sdk crate, attaches it, finds it served on
// the grid, consents to it (a stale hash is CONFLICT), stands in the grid over native UDP, trusts
// its author, fetches the module and checks it against its digest, detaches it, and deletes the
// mod.
//
//   CROWDY_E2E_API_URL, CROWDY_E2E_EMAIL, CROWDY_E2E_APP_ID (an app on ck-exec)
//   CROWDY_E2E_OWNER_EMAIL / CROWDY_E2E_OWNER_PASSWORD  the grid's owner; it need not administer
//                                                      the app
//   CROWDY_E2E_EXEC_MOD_GRID_ID  a grid that account owns, with SERVER and CLIENT code permissions
//                                there and nothing awaiting admission
//
// The API serves the module, and lets a player trust an author, only while one of that player's
// actors stands in the grid. When the replication path cannot put one there in time, both steps
// accept NOT_FOUND and say so.
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "crowdy/domains/exec.hpp"
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::domains;

namespace {

struct CheckFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Throws rather than exits, so the mod this suite deployed is deleted on the way out.
#define LIVE_CHECK(cond)                                                                        \
  do {                                                                                          \
    if (!(cond)) throw CheckFailed(std::string(#cond) + " at line " + std::to_string(__LINE__)); \
  } while (0)

const char* const kClientLib = R"rs(use crowdy_client_sdk as crowdy;

fn init() {
    crowdy::log(1, "ready");
}

fn tick(_dt_ms: u32) {
    let ticks = u32::from_le_bytes(crowdy::state_get().try_into().unwrap_or([0; 4])).wrapping_add(1);
    crowdy::state_set(&ticks.to_le_bytes());
    let _ = crowdy::api::hud_set(serde_json::json!({ "text": format!("Ticks here: {ticks}") }));
}

fn invoke(payload: &[u8]) -> Vec<u8> {
    payload.to_vec()
}

fn event(_payload: &[u8]) {}

crowdy::register_module!(init: init, tick: tick, invoke: invoke, event: event);
)rs";

std::string clientCargo(const std::string& name) {
  return "[package]\nname = \"" + name +
         "\"\nversion = \"0.1.0\"\nedition = \"2021\"\n\n[lib]\ncrate-type = [\"cdylib\"]\n\n"
         "[package.metadata.crowdy]\ntick_interval_ms = 1000\n\n"
         "[dependencies]\ncrowdy-client-sdk = \"0.1.0\"\nserde_json = \"1\"\n";
}

constexpr const char* kModPrefix = "e2e-cpp-hud-";

/// PLATFORM_BUSY means the work was never started, and its remediation says to retry.
template <typename Fn>
auto whenNotBusy(const char* what, Fn&& fn) -> decltype(fn()) {
  for (int attempt = 1;; ++attempt) {
    try {
      return fn();
    } catch (const graphql::CrowdyGraphQLError& e) {
      if (e.code() != "PLATFORM_BUSY" || attempt >= 5) throw;
      std::printf("  (%s: PLATFORM_BUSY, retrying)\n", what);
      std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
    }
  }
}

bool isHexDigest(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

/// The mod the run deployed, deleted when the run ends however it ends.
struct Run {
  const e2e::E2eConfig& cfg;
  e2e::Player& owner;
  std::string gridId;
  std::string name;
  bool deployed = false;

  ExecAPI& exec() const { return owner.game->exec(); }
  const std::string& appId() const { return cfg.appId; }

  ~Run() {
    if (owner.conn) owner.conn->disconnect();
    if (!deployed) return;
    try {
      exec().modDelete(appId(), gridId, name);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "(could not delete mod %s: %s)\n", name.c_str(), e.what());
    }
  }

  graphql::Json served(const std::string& modId) const {
    graphql::Json found;
    exec().gridClientMods(appId(), gridId).forEach([&](graphql::Json m) {
      if (m["modId"].asString() == modId) found = m;
    });
    return found;
  }

  /// Put one of the owner's actors in `chunk` over native UDP. False when it cannot.
  bool standIn(const wire::ChunkCoord& chunk) {
    replication::Config rc;
    rc.appId = std::strtoll(appId().c_str(), nullptr, 10);
    rc.token = owner.tokenInfo();
    owner.conn = owner.game->replication().connect(rc);
    bool connected = false;
    for (int i = 0; i < 5 && !connected; ++i) {
      connected = owner.conn->connect().ok();
      if (!connected) std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    for (int i = 0; connected && i < 100 && owner.conn->state() != replication::ConnState::Connected; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      owner.conn->poll();
    }
    if (!connected || owner.conn->state() != replication::ConnState::Connected) return false;
    return e2e::warmUp(*owner.conn, chunk);
  }
};

bool unserved(const graphql::CrowdyHttpError& e) {
  return e.status() == 400 && (e.body().find("Cannot query field") != std::string::npos ||
                               e.body().find("GRAPHQL_VALIDATION_FAILED") != std::string::npos);
}

int body(Run& run) {
  ExecAPI& exec = run.exec();
  const std::string& appId = run.appId();
  const std::string& gridId = run.gridId;

  E2E_SUBTEST("the API serves CLIENT halves");
  try {
    exec.gridClientMods(appId, gridId);
  } catch (const graphql::CrowdyHttpError& e) {
    if (!unserved(e)) throw;
    std::puts("the API does not serve CLIENT halves yet (no execGridClientMods); skipping");
    return 77;
  }
  // A run that was killed leaves its mod behind; delete those first.
  exec.mods(appId, gridId).forEach([&](graphql::Json m) {
    const std::string stale = m["name"].asString();
    if (stale.rfind(kModPrefix, 0) != 0 || stale == run.name) return;
    try {
      exec.modDelete(appId, gridId, stale);
      std::printf("  deleted mod %s left by an earlier run\n", stale.c_str());
    } catch (const std::exception& e) {
      std::printf("  (could not delete mod %s: %s)\n", stale.c_str(), e.what());
    }
  });

  E2E_SUBTEST("the mod the CLIENT half rides: the mod starter, switched on");
  const graphql::Json starter = exec.modStarter(appId);
  ExecCrate serverCrate{run.name, {}};
  starter["files"].forEach([&](graphql::Json f) {
    serverCrate.files.emplace_back(f["path"].asString(), f["content"].asString());
  });
  const std::string serverBuildId =
      whenNotBusy("execModBuild", [&] { return exec.modBuild(appId, serverCrate); })["buildId"].asString();
  const graphql::Json serverBuild = exec.waitForModBuild(appId, serverBuildId);
  if (serverBuild["status"].asString() != "succeeded") {
    std::fprintf(stderr, "mod build log:\n%s\n", serverBuild["log"].asString().c_str());
  }
  LIVE_CHECK(serverBuild["status"].asString() == "succeeded");
  LIVE_CHECK(serverBuild["kind"].asString() == "exec");
  run.deployed = true;
  whenNotBusy("execModDeploy", [&] { return exec.modDeploy(appId, gridId, run.name, serverBuildId); });
  const graphql::Json mod =
      whenNotBusy("execModSetEnabled", [&] { return exec.modSetEnabled(appId, gridId, run.name, true); });
  if (mod["blocked"].isString()) {
    std::printf("mod %s is held (%s); nothing of it is served; skipping\n", run.name.c_str(),
                mod["blocked"].asString().c_str());
    return 77;
  }

  E2E_SUBTEST("a crowdy-client-sdk crate built as a CLIENT half");
  const std::string crate = run.name + "-client";
  const ExecCrate clientCrate{crate, {{"Cargo.toml", clientCargo(crate)}, {"src/lib.rs", kClientLib}}};
  const graphql::Json queued =
      whenNotBusy("execModClientBuild", [&] { return exec.modClientBuild(appId, clientCrate); });
  LIVE_CHECK(queued["kind"].asString() == "client");
  const graphql::Json built = exec.waitForModBuild(appId, queued["buildId"].asString());
  if (built["status"].asString() != "succeeded") {
    std::fprintf(stderr, "CLIENT build log:\n%s\n", built["log"].asString().c_str());
  }
  LIVE_CHECK(built["status"].asString() == "succeeded");
  LIVE_CHECK(built["kind"].asString() == "client");
  const graphql::Json artifact = built["artifacts"].at(0);
  LIVE_CHECK(artifact["tickIntervalMs"].asInt64() == 1000);
  LIVE_CHECK(isHexDigest(artifact["capabilityHash"].asString()));
  const auto summary = parseExecClientCapabilitySummary(artifact["capabilitySummaryJson"].asStringView());
  LIVE_CHECK(summary.has_value());
  bool hud = false;
  for (const auto& fn : summary->hostFunctions) hud = hud || fn == "hud_set";
  LIVE_CHECK(hud);
  for (const auto& import : summary->imports) {
    // A CLIENT half imports only the CLIENT ABI.
    LIVE_CHECK(import.rfind("ck.", 0) == 0 || import == "wasi_snapshot_preview1.random_get");
  }

  E2E_SUBTEST("attach it to the mod");
  graphql::Json attached;
  try {
    attached = whenNotBusy("execModClientDeploy", [&] {
      return exec.modClientDeploy(appId, gridId, run.name, built["buildId"].asString());
    });
  } catch (const graphql::CrowdyGraphQLError& e) {
    if (e.code() != "FORBIDDEN" || std::string(e.what()).find("admission") == std::string::npos) throw;
    std::printf("the app's code admission holds the CLIENT half (%s); skipping\n", e.what());
    return 77;
  }
  const std::string modId = attached["modId"].asString();
  LIVE_CHECK(attached["name"].asString() == run.name);
  LIVE_CHECK(attached["clientVersion"].asInt64() == 1);
  LIVE_CHECK(attached["digest"].asString() == artifact["digest"].asString());
  LIVE_CHECK(attached["capabilityHash"].asString() == artifact["capabilityHash"].asString());

  E2E_SUBTEST("the grid serves it, and its author consents afresh");
  const graphql::Json listed = run.served(modId);
  LIVE_CHECK(listed.isObject());
  LIVE_CHECK(listed["digest"].asString() == attached["digest"].asString());
  LIVE_CHECK(!listed["callerConsented"].asBool());
  LIVE_CHECK(listed["authorId"].asBigIntString() == attached["ownerId"].asBigIntString());
  const auto listedSummary = parseExecClientCapabilitySummary(listed["capabilitySummaryJson"].asStringView());
  LIVE_CHECK(listedSummary.has_value() && listedSummary->hostFunctions == summary->hostFunctions);
  std::optional<graphql::GraphQLOutcome> async;
  exec.gridClientModsAsync(appId, gridId, [&](graphql::GraphQLOutcome out) { async = std::move(out); });
  for (int i = 0; i < 1000 && !async; ++i) {
    run.owner.game->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  LIVE_CHECK(async.has_value() && async->ok() && async->data.isArray());

  E2E_SUBTEST("consent: a stale hash is CONFLICT, the shown one holds");
  bool conflict = false;
  try {
    exec.consentClientMod(appId, modId, std::string(64, '0'));
  } catch (const graphql::CrowdyGraphQLError& e) {
    conflict = e.code() == "CONFLICT";
  }
  LIVE_CHECK(conflict);
  LIVE_CHECK(exec.consentClientMod(appId, modId, attached["capabilityHash"].asString()).asBool());
  LIVE_CHECK(run.served(modId)["callerConsented"].asBool());

  E2E_SUBTEST("stand in the grid, then fetch the module and check it against its digest");
  const graphql::Json box = run.owner.game->grids().mintToken(appId, gridId);
  const wire::ChunkCoord inside{box["lowChunk"]["x"].asBigInt(), box["lowChunk"]["y"].asBigInt(),
                                box["lowChunk"]["z"].asBigInt()};
  const bool standing = run.standIn(inside);
  std::printf("  an actor in chunk (%lld,%lld,%lld): %s\n", static_cast<long long>(inside.x),
              static_cast<long long>(inside.y), static_cast<long long>(inside.z),
              standing ? "acknowledged" : "not acknowledged");
  std::optional<ExecModClientArtifactBytes> fetched;
  // At most 12 fetches a minute per player and mod: presence has about a minute to arrive.
  for (int attempt = 0; attempt < 11 && !fetched; ++attempt) {
    try {
      fetched = exec.modClientArtifactBytes(appId, modId);
    } catch (const graphql::CrowdyGraphQLError& e) {
      LIVE_CHECK(e.code() == "NOT_FOUND");
      if (!standing) break;
      // Keep the actor fresh for about five seconds before asking again.
      for (int i = 0; i < 5; ++i) {
        e2e::warmUp(*run.owner.conn, inside, 1000);
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }
  if (fetched) {
    LIVE_CHECK(fetched->digest == attached["digest"].asString());
    LIVE_CHECK(static_cast<std::int64_t>(fetched->bytes.size()) == attached["sizeBytes"].asInt64());
    LIVE_CHECK(fetched->bytes.size() >= 4 && fetched->bytes[0] == 0 && fetched->bytes[1] == 'a' &&
               fetched->bytes[2] == 's' && fetched->bytes[3] == 'm');
    LIVE_CHECK(!fetched->fuelPerDispatch.empty() && fetched->fuelPerDispatch != "0");
    LIVE_CHECK(fetched->abiVersion == kExecClientAbiVersion);
    LIVE_CHECK(fetched->tickIntervalMs == 1000);
    LIVE_CHECK(fetched->capabilitySummary.hostFunctions == summary->hostFunctions);
    std::printf("  served %zu bytes, SHA-256 %s, fuel per dispatch %s\n", fetched->bytes.size(),
                fetched->digest.c_str(), fetched->fuelPerDispatch.c_str());
  } else {
    std::puts("  modClientArtifactBytes: NOT_FOUND (no actor of ours in the grid reached presence)");
  }

  E2E_SUBTEST("trust its author");
  try {
    LIVE_CHECK(exec.trustAuthor(appId, gridId, listed["authorId"].asBigIntString(),
                                listed["authorCapabilityHash"].asString())
                   .asBool());
    LIVE_CHECK(run.served(modId)["callerTrustsAuthor"].asBool());
  } catch (const graphql::CrowdyGraphQLError& e) {
    // Only a player standing in the grid can trust its authors there.
    LIVE_CHECK(e.code() == "NOT_FOUND" && !fetched);
    std::puts("  trustAuthor: NOT_FOUND (not standing in the grid)");
  }

  E2E_SUBTEST("detach it: no longer served");
  LIVE_CHECK(exec.modClientDelete(appId, gridId, run.name).asBool());
  LIVE_CHECK(!run.served(modId).isObject());

  std::puts("e2e_exec_client_halves OK");
  return 0;
}

}  // namespace

int main() {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  const std::string gridId = e2e::envOr("CROWDY_E2E_EXEC_MOD_GRID_ID");
  if (gridId.empty()) {
    std::puts("CROWDY_E2E_EXEC_MOD_GRID_ID not configured; skipping");
    return 77;
  }
  // The owner's app token, kept: the same one signs the UDP session that stands in the grid.
  e2e::Player owner;
  owner.appToken = e2e::owner(cfg).portal().mintAppToken(cfg.appId);
  ClientConfig game;
  game.httpUrl = !cfg.httpUrl.empty() ? cfg.httpUrl
                                      : (!owner.appToken.gameApiUrl.empty() ? owner.appToken.gameApiUrl.valueOrEmpty()
                                                                            : cfg.apiUrl);
  owner.game = std::make_unique<CrowdyClient>(std::move(game));
  owner.game->setToken(owner.appToken.token);

  Run run{cfg, owner, gridId, "e2e-cpp-hud-" + e2e::runSuffix()};
  try {
    return body(run);
  } catch (const CheckFailed& e) {
    std::fprintf(stderr, "LIVE_CHECK failed: %s\n", e.what());
  } catch (const graphql::CrowdyGraphQLError& e) {
    e2e::printGraphQLError(e, "e2e_exec_client_halves");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_exec_client_halves: %s\n", e.what());
  }
  return 1;
}
