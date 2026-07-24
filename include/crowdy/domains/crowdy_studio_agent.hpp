#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

namespace crowdy::domains {

/// Exact GraphQL surface for `crowdy.studio-agent/1`.
///
/// Runtime/session operations are sent only to the Game API. Policy, usage,
/// and operator controls are sent only to the Management API. This class does
/// not expose an arbitrary document executor, provider client, UDP authority,
/// or tool bridge.
class CrowdyStudioAgentAPI {
 public:
  CrowdyStudioAgentAPI(std::shared_ptr<graphql::GraphQLClient> game,
                       std::shared_ptr<graphql::GraphQLClient> management,
                       std::shared_ptr<graphql::Dispatcher> dispatcher)
      : game_(std::move(game)),
        management_(std::move(management)),
        dispatcher_(std::move(dispatcher)) {}

  std::shared_ptr<graphql::Dispatcher> dispatcher() const { return dispatcher_; }

  // Game API reads.
  graphql::Json session(std::string_view sessionId) const {
    return game("CrowdyStudioAgentSession", one("sessionId", sessionId));
  }
  void sessionAsync(std::string_view sessionId, graphql::GraphQLCallback cb) const {
    gameAsync("CrowdyStudioAgentSession", one("sessionId", sessionId),
              std::move(cb));
  }
  graphql::Json sessions(std::string_view appId,
                         const graphql::JVal& page = graphql::JVal()) const {
    auto vars = page;
    vars["appId"] = appId;
    return game("CrowdyStudioAgentSessions", vars);
  }
  void sessionsAsync(std::string_view appId, const graphql::JVal& page,
                     graphql::GraphQLCallback cb) const {
    auto vars = page;
    vars["appId"] = appId;
    gameAsync("CrowdyStudioAgentSessions", vars, std::move(cb));
  }
  graphql::Json history(std::string_view sessionId, std::string_view afterSeq = "0",
                        int first = 100) const {
    graphql::JVal vars;
    vars["sessionId"] = sessionId;
    vars["afterSeq"] = afterSeq;
    vars["first"] = std::int64_t{first};
    return game("CrowdyStudioAgentHistory", vars);
  }
  void historyAsync(std::string_view sessionId, std::string_view afterSeq,
                    int first, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["sessionId"] = sessionId;
    vars["afterSeq"] = afterSeq;
    vars["first"] = std::int64_t{first};
    gameAsync("CrowdyStudioAgentHistory", vars, std::move(cb));
  }
  graphql::Json toolDescriptors(std::string_view sessionId) const {
    return game("CrowdyStudioAgentToolDescriptors",
                one("sessionId", sessionId));
  }
  void toolDescriptorsAsync(std::string_view sessionId,
                            graphql::GraphQLCallback cb) const {
    gameAsync("CrowdyStudioAgentToolDescriptors", one("sessionId", sessionId),
              std::move(cb));
  }
  graphql::Json budget(std::string_view sessionId) const {
    return game("CrowdyStudioAgentBudget", one("sessionId", sessionId));
  }
  void budgetAsync(std::string_view sessionId,
                   graphql::GraphQLCallback cb) const {
    gameAsync("CrowdyStudioAgentBudget", one("sessionId", sessionId),
              std::move(cb));
  }

  // Game API mutations. Inputs deliberately mirror the named schema inputs;
  // authenticated authority fields are injected/validated by the server.
  graphql::Json createSession(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentCreateSession", input);
  }
  void createSessionAsync(const graphql::JVal& input,
                          graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentCreateSession", input, std::move(cb));
  }
  graphql::Json attachClient(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentAttachClient", input);
  }
  void attachClientAsync(const graphql::JVal& input,
                         graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentAttachClient", input, std::move(cb));
  }
  graphql::Json setMode(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentSetMode", input);
  }
  void setModeAsync(const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentSetMode", input, std::move(cb));
  }
  graphql::Json acknowledgeEvents(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentAcknowledgeEvents", input);
  }
  void acknowledgeEventsAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentAcknowledgeEvents", input, std::move(cb));
  }
  graphql::Json heartbeat(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentHeartbeat", input);
  }
  void heartbeatAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentHeartbeat", input, std::move(cb));
  }
  graphql::Json sendMessage(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentSendMessage", input);
  }
  void sendMessageAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentSendMessage", input, std::move(cb));
  }
  graphql::Json approveTool(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentApproveTool", input);
  }
  void approveToolAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentApproveTool", input, std::move(cb));
  }
  graphql::Json rejectTool(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentRejectTool", input);
  }
  void rejectToolAsync(const graphql::JVal& input,
                       graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentRejectTool", input, std::move(cb));
  }
  graphql::Json browserToolResult(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentToolResult", input);
  }
  void browserToolResultAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentToolResult", input, std::move(cb));
  }
  graphql::Json grantLease(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentGrantLease", input);
  }
  void grantLeaseAsync(const graphql::JVal& input,
                       graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentGrantLease", input, std::move(cb));
  }
  graphql::Json revokeLease(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentRevokeLease", input);
  }
  void revokeLeaseAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentRevokeLease", input, std::move(cb));
  }
  graphql::Json pause(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentPause", input);
  }
  void pauseAsync(const graphql::JVal& input,
                  graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentPause", input, std::move(cb));
  }
  graphql::Json resume(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentResume", input);
  }
  void resumeAsync(const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentResume", input, std::move(cb));
  }
  graphql::Json cancelRun(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentCancelRun", input);
  }
  void cancelRunAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentCancelRun", input, std::move(cb));
  }
  graphql::Json closeSession(const graphql::JVal& input) const {
    return gameInput("CrowdyStudioAgentCloseSession", input);
  }
  void closeSessionAsync(const graphql::JVal& input,
                         graphql::GraphQLCallback cb) const {
    gameInputAsync("CrowdyStudioAgentCloseSession", input, std::move(cb));
  }

  // Management API app policy and sanitized usage.
  graphql::Json policy(std::string_view appId) const {
    return management("CrowdyStudioAgentPolicy", one("appId", appId));
  }
  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    managementAsync("CrowdyStudioAgentPolicy", one("appId", appId),
                    std::move(cb));
  }
  graphql::Json effectivePolicy(std::string_view appId) const {
    return management("CrowdyStudioAgentEffectivePolicy", one("appId", appId));
  }
  void effectivePolicyAsync(std::string_view appId,
                            graphql::GraphQLCallback cb) const {
    managementAsync("CrowdyStudioAgentEffectivePolicy", one("appId", appId),
                    std::move(cb));
  }
  graphql::Json usage(std::string_view appId,
                      const graphql::JVal& window = graphql::JVal()) const {
    auto vars = window;
    vars["appId"] = appId;
    return management("CrowdyStudioAgentUsage", vars);
  }
  void usageAsync(std::string_view appId, const graphql::JVal& window,
                  graphql::GraphQLCallback cb) const {
    auto vars = window;
    vars["appId"] = appId;
    managementAsync("CrowdyStudioAgentUsage", vars, std::move(cb));
  }
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return managementInput("CrowdyStudioAgentSetPolicy", input);
  }
  void setPolicyAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    managementInputAsync("CrowdyStudioAgentSetPolicy", input, std::move(cb));
  }

  // Management API operator roots.
  graphql::Json platformPolicy() const {
    return management("CpCrowdyStudioAgentPlatformPolicy", graphql::JVal());
  }
  void platformPolicyAsync(graphql::GraphQLCallback cb) const {
    managementAsync("CpCrowdyStudioAgentPlatformPolicy", graphql::JVal(),
                    std::move(cb));
  }
  graphql::Json setPlatformPolicy(const graphql::JVal& input) const {
    return managementInput("CpSetCrowdyStudioAgentPlatformPolicy", input);
  }
  void setPlatformPolicyAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    managementInputAsync("CpSetCrowdyStudioAgentPlatformPolicy", input,
                         std::move(cb));
  }
  graphql::Json setOperatorAppKill(const graphql::JVal& input) const {
    return managementInput("CpSetCrowdyStudioAgentAppKill", input);
  }
  void setOperatorAppKillAsync(const graphql::JVal& input,
                               graphql::GraphQLCallback cb) const {
    managementInputAsync("CpSetCrowdyStudioAgentAppKill", input, std::move(cb));
  }

 private:
  static graphql::JVal one(std::string_view key, std::string_view value) {
    graphql::JVal vars;
    vars[key] = value;
    return vars;
  }
  static graphql::JVal inputVars(const graphql::JVal& input) {
    graphql::JVal vars;
    vars["input"] = input;
    return vars;
  }
  graphql::Json game(std::string_view operation,
                     const graphql::JVal& vars) const {
    auto data = game_->request(gen::crowdyStudioAgent::documentFor(operation),
                               vars, operation);
    graphql::Json result;
    data.forEachMember([&](std::string_view, graphql::Json value) {
      result = value;
    });
    return result;
  }
  void gameAsync(std::string_view operation, const graphql::JVal& vars,
                 graphql::GraphQLCallback cb) const {
    unwrapAsync(game_, gen::crowdyStudioAgent::documentFor(operation),
                operation, vars, std::move(cb));
  }
  graphql::Json gameInput(std::string_view operation,
                          const graphql::JVal& input) const {
    return game(operation, inputVars(input));
  }
  void gameInputAsync(std::string_view operation, const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    gameAsync(operation, inputVars(input), std::move(cb));
  }
  graphql::Json management(std::string_view operation,
                           const graphql::JVal& vars) const {
    auto data = management_->request(
        gen::crowdyStudioAgent::documentFor(operation), vars,
        operation);
    graphql::Json result;
    data.forEachMember([&](std::string_view, graphql::Json value) {
      result = value;
    });
    return result;
  }
  void managementAsync(std::string_view operation, const graphql::JVal& vars,
                       graphql::GraphQLCallback cb) const {
    unwrapAsync(
        management_,
        gen::crowdyStudioAgent::documentFor(operation),
        operation, vars, std::move(cb));
  }
  graphql::Json managementInput(std::string_view operation,
                                const graphql::JVal& input) const {
    return management(operation, inputVars(input));
  }
  void managementInputAsync(std::string_view operation,
                            const graphql::JVal& input,
                            graphql::GraphQLCallback cb) const {
    managementAsync(operation, inputVars(input), std::move(cb));
  }
  static void unwrapAsync(
      const std::shared_ptr<graphql::GraphQLClient>& client,
      std::string_view document, std::string_view operation,
      const graphql::JVal& vars, graphql::GraphQLCallback cb) {
    client->requestAsync(
        document, vars, operation,
        [cb = std::move(cb)](graphql::GraphQLOutcome outcome) mutable {
          if (outcome.ok() && outcome.data.isObject() &&
              outcome.data.size() == 1) {
            graphql::Json result;
            outcome.data.forEachMember(
                [&](std::string_view, graphql::Json value) { result = value; });
            outcome.data = result;
          }
          cb(std::move(outcome));
        });
  }

  std::shared_ptr<graphql::GraphQLClient> game_;
  std::shared_ptr<graphql::GraphQLClient> management_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
};

}  // namespace crowdy::domains
