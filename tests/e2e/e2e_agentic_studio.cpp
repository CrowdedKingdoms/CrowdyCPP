#include <string>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

struct AttachedSession {
  std::string sessionId;
  std::string clientEpoch;
};

AttachedSession createAndAttach(
    domains::CrowdyStudioAgentAPI& api, std::string_view appId,
    std::string_view mode, std::string_view suffix,
    std::optional<std::string> projectId = std::nullopt,
    std::optional<std::string> gridId = std::nullopt) {
  graphql::JVal create;
  create["appId"] = appId;
  create["mode"] = mode;
  create["providerDataConsent"] = false;
  create["idempotencyKey"] =
      "cpp-agent-create-" + std::string(mode) + "-" + std::string(suffix);
  if (projectId) create["projectId"] = *projectId;
  if (gridId) create["gridId"] = *gridId;
  const auto session = api.createSession(create);
  AttachedSession result;
  result.sessionId = session["sessionId"].asString();
  E2E_CHECK(!result.sessionId.empty());

  graphql::JVal attach;
  attach["sessionId"] = result.sessionId;
  attach["clientInstanceId"] =
      "cpp-e2e-" + std::string(mode) + "-" + std::string(suffix);
  attach["idempotencyKey"] =
      "cpp-agent-attach-" + std::string(mode) + "-" + std::string(suffix);
  const auto attached = api.attachClient(attach);
  result.clientEpoch = attached["clientEpoch"].isString()
                           ? attached["clientEpoch"].asString()
                           : attached["clientEpoch"].dump();
  E2E_CHECK(!result.clientEpoch.empty());
  return result;
}

void closeSession(domains::CrowdyStudioAgentAPI& api,
                  const AttachedSession& session,
                  std::string_view suffix) {
  graphql::JVal close;
  close["sessionId"] = session.sessionId;
  close["clientEpoch"] = session.clientEpoch;
  close["idempotencyKey"] =
      "cpp-agent-close-" + std::string(suffix) + "-" + session.sessionId;
  const auto closed = api.closeSession(close);
  E2E_CHECK(closed["status"].asString() == "CLOSED");
}

}  // namespace

int main() {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  e2e::requireFlag("CROWDY_E2E_AGENT");
  const std::string projectId =
      e2e::envOr("CROWDY_E2E_AGENT_PROJECT_ID");
  const std::string gridId = e2e::envOr("CROWDY_E2E_STUDIO_GRID_ID");
  if (projectId.empty() || gridId.empty()) {
    std::puts(
        "CROWDY_E2E_AGENT_PROJECT_ID/STUDIO_GRID_ID not configured; skipping");
    return 77;
  }

  auto& api = e2e::ownerGame(cfg).crowdyStudioAgent();
  const std::string suffix = e2e::runSuffix();

  E2E_SUBTEST("create, attach, and close ASK");
  const auto ask = createAndAttach(api, cfg.appId, "ASK", suffix);
  if (e2e::envFlag("CROWDY_E2E_AGENT_RUN")) {
    graphql::JVal message;
    message["sessionId"] = ask.sessionId;
    message["clientEpoch"] = ask.clientEpoch;
    message["idempotencyKey"] = "cpp-agent-ask-run-" + suffix;
    message["content"] = "Reply with one short readiness sentence.";
    const auto run = api.sendMessage(message);
    E2E_CHECK(!run["runId"].asString().empty());
  }
  closeSession(api, ask, suffix + "-ask");

  E2E_SUBTEST("create and attach BUILD to an exact saved project");
  const auto build = createAndAttach(api, cfg.appId, "BUILD", suffix,
                                     projectId, gridId);
  E2E_CHECK(api.session(build.sessionId)["mode"].asString() == "BUILD");
  closeSession(api, build, suffix + "-build");

  if (e2e::envFlag("CROWDY_E2E_AGENT_PLAY")) {
    const std::string controlled =
        e2e::envOr("CROWDY_E2E_AGENT_CONTROLLED_ENTITY_ID");
    const std::string capability =
        e2e::envOr("CROWDY_E2E_AGENT_HOST_CAPABILITY_REVISION");
    if (controlled.empty() || capability.empty()) {
      std::puts(
          "Play host entity/capability vars missing; skipping Play subtest");
    } else {
      E2E_SUBTEST("grant Play observe/control then human-takeover revoke");
      const auto play =
          createAndAttach(api, cfg.appId, "PLAY", suffix, std::nullopt, gridId);
      graphql::JVal grant;
      grant["sessionId"] = play.sessionId;
      grant["clientEpoch"] = play.clientEpoch;
      grant["idempotencyKey"] = "cpp-agent-play-grant-" + suffix;
      grant["scopes"] =
          graphql::JVal::array({graphql::JVal("observe"),
                                graphql::JVal("locomotion")});
      grant["durationSeconds"] = 30;
      grant["controlledEntityId"] = controlled;
      grant["hostCapabilityRevision"] = capability;
      const auto lease = api.grantLease(grant);
      E2E_CHECK(lease["status"].asString() == "ACTIVE");

      graphql::JVal revoke;
      revoke["sessionId"] = play.sessionId;
      revoke["clientEpoch"] = play.clientEpoch;
      revoke["idempotencyKey"] = "cpp-agent-play-revoke-" + suffix;
      revoke["leaseId"] = lease["leaseId"].asString();
      revoke["reason"] = "HUMAN_INPUT";
      const auto revoked = api.revokeLease(revoke);
      E2E_CHECK(revoked["status"].asString() == "REVOKED");
      E2E_CHECK(revoked["revokedReason"].asString() == "HUMAN_INPUT");
      closeSession(api, play, suffix + "-play");
    }
  }

  if (e2e::envFlag("CROWDY_E2E_AGENT_POLICY_KILL")) {
    if (cfg.operatorEmail.empty()) {
      std::puts("CROWDY_E2E_OPERATOR_EMAIL missing; skipping kill subtest");
    } else {
      E2E_SUBTEST("publish and release an operator policy kill");
      auto operatorClient =
          e2e::identityClient(cfg, cfg.operatorEmail);
      auto& operatorApi = operatorClient->crowdyStudioAgent();
      graphql::JVal kill;
      kill["appId"] = cfg.appId;
      kill["killed"] = true;
      kill["reasonCode"] = "AGENT_OPERATOR_KILLED";
      kill["reason"] = "CrowdyCPP explicit e2e policy-kill check";
      kill["idempotencyKey"] = "cpp-agent-kill-" + suffix;
      const bool killed =
          operatorApi.setOperatorAppKill(kill)["operatorKillSwitch"]
              .asBool(false);

      graphql::JVal release;
      release["appId"] = cfg.appId;
      release["killed"] = false;
      release["reasonCode"] = "AGENT_OPERATOR_KILLED";
      release["reason"] = "CrowdyCPP e2e policy-kill release";
      release["idempotencyKey"] = "cpp-agent-kill-release-" + suffix;
      const bool released =
          !operatorApi.setOperatorAppKill(release)["operatorKillSwitch"]
               .asBool(true);
      E2E_CHECK(killed);
      E2E_CHECK(released);
    }
  }

  std::puts("e2e_agentic_studio passed");
  return 0;
}
