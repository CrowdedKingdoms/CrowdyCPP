// Optional live evidence for a Studio dock's platform calls (0.86.0), as the owner of a grid in a
// ck-exec app: the live-modding check (a refused action carries its code), the model list, the
// tier's Studio origin, a starter project created without blocking, and CrowdyStudioModRunner's
// draft tests: a full-stack project (CLIENT half built first, SERVER half deployed and switched
// on, CLIENT half attached and consented to) and a CLIENT-only project, which rides a mod of its
// CLIENT module's name. Every callback arrives through poll().
//
//   CROWDY_E2E_API_URL, CROWDY_E2E_EMAIL, CROWDY_E2E_APP_ID (an app on ck-exec)
//   CROWDY_E2E_OWNER_EMAIL / CROWDY_E2E_OWNER_PASSWORD  the grid's owner
//   CROWDY_E2E_EXEC_MOD_GRID_ID  a grid that account owns, with SERVER and CLIENT code permissions
//                                there and nothing awaiting admission
//   CROWDY_E2E_AGENT_ENABLED=1   the app's agent policy is on for that account: require the
//                                live-modding check to pass and the model list to name a model.
//                                Unset, a documented refusal of either is reported and accepted.
//
// The mods and projects it makes are named `e2e-dock-<run>` and removed when it ends.
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/domains/crowdy_studio.hpp"
#include "crowdy/domains/crowdy_studio_agent.hpp"
#include "crowdy/domains/exec.hpp"
#include "crowdy/studio/mod_runner.hpp"
#include "crowdy/studio/starter_projects.hpp"
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::domains;

namespace {

struct CheckFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Throws rather than exits, so what this suite made is removed on the way out.
#define LIVE_CHECK(cond)                                                                        \
  do {                                                                                          \
    if (!(cond)) throw CheckFailed(std::string(#cond) + " at line " + std::to_string(__LINE__)); \
  } while (0)

constexpr const char* kPrefix = "e2e-dock-";

bool isHexDigest(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool documentedAgentRefusal(const std::string& code) {
  for (const char* known : {"AGENT_SCOPE_DENIED", "AGENT_PERMISSION_DENIED", "AGENT_DISABLED",
                            "AGENT_OPERATOR_KILLED", "AGENT_PROVIDER_POLICY_UNSATISFIED"}) {
    if (code == known) return true;
  }
  return false;
}

/// What the run made, removed when it ends however it ends.
struct Session {
  const e2e::E2eConfig& cfg;
  CrowdyClient& game;
  std::string gridId;
  std::vector<std::string> mods;
  std::vector<studio::CrowdyStudioProject> projects;

  ~Session() {
    for (const auto& name : mods) {
      try {
        game.exec().modDelete(cfg.appId, gridId, name);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "(could not delete mod %s: %s)\n", name.c_str(), e.what());
      }
    }
    for (const auto& project : projects) {
      try {
        studio::SetCrowdyStudioProjectArchivedInput archive;
        archive.appId = cfg.appId;
        archive.projectId = project.projectId;
        archive.expectedRevisionId = game.crowdyStudio().getProject({cfg.appId, gridId}, project.projectId).revision.id;
        archive.archived = true;
        game.crowdyStudio().setProjectArchived(archive);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "(could not archive project %s: %s)\n", project.projectId.c_str(), e.what());
      }
    }
  }

  /// Poll the client until `done` holds, or `seconds` pass.
  template <typename Predicate>
  bool pollUntil(Predicate done, int seconds) {
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done() && std::chrono::steady_clock::now() < giveUp) {
      game.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
  }

  studio::CrowdyStudioProject create(const std::string& name, studio::CrowdyStudioProjectKind kind) {
    studio::CrowdyStudioNewProjectOptions options;
    options.appId = cfg.appId;
    options.gridId = gridId;
    options.name = name;
    options.kind = kind;
    std::optional<graphql::GraphQLOutcome> outcome;
    studio::CrowdyStudioProject created;
    studio::createCrowdyStudioStarterProjectAsync(
        game.exec(), game.crowdyStudio(), options,
        [&](graphql::GraphQLOutcome out, studio::CrowdyStudioProject project) {
          outcome = std::move(out);
          created = std::move(project);
        });
    LIVE_CHECK(pollUntil([&] { return outcome.has_value(); }, 60));
    if (!outcome->ok()) {
      throw CheckFailed("creating " + name + " failed: " +
                        (outcome->errors.empty() ? outcome->errorMessage : outcome->errors.front().message));
    }
    projects.push_back(created);
    return created;
  }

  /// One draft test, retried while the platform says to wait (one build per player, a full queue).
  studio::CrowdyStudioRunResult draft(studio::CrowdyStudioModRunner& runner, const studio::CrowdyStudioProject& project) {
    for (int attempt = 1;; ++attempt) {
      studio::CrowdyStudioRunRequest request;
      request.scope = {cfg.appId, gridId};
      request.projectId = project.projectId;
      request.expectedRevisionId = project.revision.id;
      request.pollIntervalMs = 2'000;
      std::optional<studio::CrowdyStudioRunResult> result;
      runner.run(
          request, [&](studio::CrowdyStudioRunResult done) { result = std::move(done); },
          [](const studio::CrowdyStudioRunProgress& progress) {
            std::printf("    %s %s\n", std::string(studio::toString(progress.step)).c_str(), progress.message.c_str());
          });
      LIVE_CHECK(pollUntil([&] { return result.has_value(); }, 900));
      const bool wait = result->outcome == studio::CrowdyStudioRunOutcome::RateLimited ||
                        result->outcome == studio::CrowdyStudioRunOutcome::QueueFull;
      if (!wait || attempt >= 4) return *result;
      std::printf("  (%s: %s; waiting)\n", std::string(studio::toString(result->outcome)).c_str(),
                  result->message.c_str());
      std::this_thread::sleep_for(std::chrono::seconds(20 * attempt));
    }
  }
};

void printTargets(const studio::CrowdyStudioRunResult& result) {
  for (const auto& target : result.targets) {
    std::printf("  %s %s: build %s %s, digest %s, version %lld\n", std::string(studio::toString(target.target)).c_str(),
                target.moduleName.c_str(), target.buildId.c_str(), target.compileStatus.c_str(),
                target.digest.c_str(), static_cast<long long>(target.version));
    if (target.compileStatus == "failed") std::fprintf(stderr, "%s\n", target.buildLog.c_str());
  }
}

int body(Session& session) {
  const std::string& appId = session.cfg.appId;
  CrowdyClient& game = session.game;
  CrowdyStudioAgentAPI& agent = game.crowdyStudioAgent();
  const bool agentEnabled = e2e::envFlag("CROWDY_E2E_AGENT_ENABLED");

  // A run that was killed leaves its mods behind; delete those first.
  game.exec().mods(appId, session.gridId).forEach([&](graphql::Json m) {
    const std::string stale = m["name"].asString();
    if (stale.rfind(kPrefix, 0) != 0) return;
    try {
      game.exec().modDelete(appId, session.gridId, stale);
      std::printf("  deleted mod %s left by an earlier run\n", stale.c_str());
    } catch (const std::exception& e) {
      std::printf("  (could not delete mod %s: %s)\n", stale.c_str(), e.what());
    }
  });

  E2E_SUBTEST("the live-modding check");
  try {
    LIVE_CHECK(agent.liveModding(appId, CrowdyStudioLiveModdingAction::DraftTest));
    std::puts("  draft_test: allowed");
  } catch (const graphql::CrowdyGraphQLError& e) {
    std::printf("  draft_test: %s (%s)\n", e.code().c_str(), e.what());
    LIVE_CHECK(!agentEnabled && documentedAgentRefusal(e.code()));
  }
  std::optional<graphql::GraphQLOutcome> checked;
  agent.liveModdingAsync(appId, "not_an_action", [&](graphql::GraphQLOutcome out) { checked = std::move(out); });
  LIVE_CHECK(session.pollUntil([&] { return checked.has_value(); }, 30));
  LIVE_CHECK(checked->kind == graphql::GraphQLErrorKind::GraphQL);
  LIVE_CHECK(checked->errors.at(0).code == "AGENT_SCOPE_DENIED");

  E2E_SUBTEST("the model list");
  const CrowdyStudioModelList models = agent.models(appId);
  if (models.ok()) {
    std::printf("  %zu model(s)\n", models.models.size());
    for (const auto& model : models.models) {
      LIVE_CHECK(!model.id.empty() && !model.name.empty());
      LIVE_CHECK(model.pricingMicrousdPerMillion.input > 0 && model.pricingMicrousdPerMillion.output > 0);
      std::printf("    %s: %lld in, %lld out micro-USD per million\n", model.id.c_str(),
                  static_cast<long long>(model.pricingMicrousdPerMillion.input),
                  static_cast<long long>(model.pricingMicrousdPerMillion.output));
    }
    LIVE_CHECK(!agentEnabled || !models.models.empty());
  } else {
    LIVE_CHECK(models.error.has_value());
    std::printf("  refused: HTTP %d %s (%s), retryable %d\n", models.httpStatus, models.error->code.c_str(),
                models.error->message.c_str(), models.error->retryable ? 1 : 0);
    LIVE_CHECK(!agentEnabled && documentedAgentRefusal(models.error->code));
    LIVE_CHECK(models.error->type == "crowded_kingdoms_error");
  }
  std::optional<CrowdyStudioModelList> listed;
  agent.modelsAsync(appId, [&](CrowdyStudioModelList answer) { listed = std::move(answer); });
  LIVE_CHECK(session.pollUntil([&] { return listed.has_value(); }, 30));
  LIVE_CHECK(listed->ok() == models.ok() && listed->models.size() == models.models.size());

  E2E_SUBTEST("the tier's Studio");
  const std::string studioOrigin = game.studioOrigin();
  std::printf("  API %s: Studio %s\n", game.graphqlClient().origin().c_str(),
              studioOrigin.empty() ? "(no convention for this host)" : studioOrigin.c_str());
  LIVE_CHECK(studioOrigin.empty() || studioOrigin.rfind("https://studio.", 0) == 0 ||
             studioOrigin.rfind("http://localhost:", 0) == 0 || studioOrigin.rfind("http://127.0.0.1:", 0) == 0);

  auto runner = game.createCrowdyStudioModRunner();
  const std::string base = std::string(kPrefix) + e2e::runSuffix();

  E2E_SUBTEST("a full-stack starter project, created without blocking");
  const studio::CrowdyStudioProject full = session.create(base, studio::CrowdyStudioProjectKind::FullStack);
  LIVE_CHECK(full.metadata.serverModuleName == std::optional<std::string>(base + "-server"));
  LIVE_CHECK(full.metadata.clientModuleName == std::optional<std::string>(base + "-client"));
  LIVE_CHECK(full.files.size() >= 4);
  LIVE_CHECK(full.sdkVersion == "0.1.8");
  session.mods.push_back(base + "-server");

  E2E_SUBTEST("a draft test that pins a revision the project is not at is refused");
  studio::CrowdyStudioRunRequest stale;
  stale.scope = {appId, session.gridId};
  stale.projectId = full.projectId;
  stale.expectedRevisionId = "0";
  std::optional<studio::CrowdyStudioRunResult> refused;
  runner->run(stale, [&](studio::CrowdyStudioRunResult done) { refused = std::move(done); });
  LIVE_CHECK(session.pollUntil([&] { return refused.has_value(); }, 30));
  LIVE_CHECK(refused->outcome == studio::CrowdyStudioRunOutcome::RevisionConflict);

  E2E_SUBTEST("an async draft test of both halves");
  const studio::CrowdyStudioRunResult ran = session.draft(*runner, full);
  printTargets(ran);
  if (!ran.ok()) std::fprintf(stderr, "  %s: %s\n", std::string(studio::toString(ran.outcome)).c_str(), ran.message.c_str());
  LIVE_CHECK(ran.ok());
  LIVE_CHECK(ran.projectRevisionId == full.revision.id);
  LIVE_CHECK(ran.targets.size() == 2);
  const studio::CrowdyStudioRunTarget& client = ran.targets.at(0);
  const studio::CrowdyStudioRunTarget& server = ran.targets.at(1);
  LIVE_CHECK(client.target == studio::CrowdyStudioTarget::Client && server.target == studio::CrowdyStudioTarget::Server);
  LIVE_CHECK(isHexDigest(client.digest) && client.version >= 1 && isHexDigest(client.capabilityHash));
  LIVE_CHECK(!server.digest.empty() && server.version >= 1);
  LIVE_CHECK(client.phase == studio::CrowdyStudioRunStep::Done && server.phase == studio::CrowdyStudioRunStep::Done);
  bool served = false;
  game.exec().gridClientMods(appId, session.gridId).forEach([&](graphql::Json m) {
    if (m["name"].asString() == base + "-server" && m["digest"].asString() == client.digest) {
      served = m["callerConsented"].asBool();
    }
  });
  LIVE_CHECK(served);

  E2E_SUBTEST("a CLIENT-only project rides a mod of its CLIENT module's name");
  const std::string hudName = base + "h";
  const studio::CrowdyStudioProject hud = session.create(hudName, studio::CrowdyStudioProjectKind::Client);
  session.mods.push_back(hudName + "-client");
  const studio::CrowdyStudioRunResult rode = session.draft(*runner, hud);
  printTargets(rode);
  if (!rode.ok()) std::fprintf(stderr, "  %s: %s\n", std::string(studio::toString(rode.outcome)).c_str(), rode.message.c_str());
  LIVE_CHECK(rode.ok());
  LIVE_CHECK(rode.targets.size() == 1 && rode.targets.at(0).modName == hudName + "-client");
  bool riding = false;
  game.exec().mods(appId, session.gridId).forEach([&](graphql::Json m) {
    if (m["name"].asString() == hudName + "-client") riding = m["enabled"].asBool();
  });
  LIVE_CHECK(riding);

  std::puts("e2e_studio_dock OK");
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
  const domains::AppTokenResponse token = e2e::owner(cfg).portal().mintAppToken(cfg.appId);
  ClientConfig config;
  config.httpUrl = !cfg.httpUrl.empty() ? cfg.httpUrl
                                        : (!token.gameApiUrl.empty() ? token.gameApiUrl.valueOrEmpty() : cfg.apiUrl);
  CrowdyClient game(std::move(config));
  game.setToken(token.token);

  Session session{cfg, game, gridId, {}, {}};
  try {
    return body(session);
  } catch (const CheckFailed& e) {
    std::fprintf(stderr, "LIVE_CHECK failed: %s\n", e.what());
  } catch (const graphql::CrowdyGraphQLError& e) {
    e2e::printGraphQLError(e, "e2e_studio_dock");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_studio_dock: %s\n", e.what());
  }
  return 1;
}
