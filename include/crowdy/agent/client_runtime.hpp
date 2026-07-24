#pragma once

#include <utility>

#include "crowdy/agent/controller.hpp"

namespace crowdy::agent {

/**
 * Lifetime-safe construction path for the production Agentic Studio client.
 * The typed HTTP transport and GraphQL-WS event adapter outlive the controller
 * and share CrowdyClient's Dispatcher.
 */
class CrowdyStudioAgentControllerRuntime {
 public:
  CrowdyStudioAgentControllerRuntime(
      domains::CrowdyStudioAgentAPI& api,
      graphql::GraphQLSubscriptionClient& subscriptions,
      CrowdyStudioAgentControllerOptions options)
      : transport_(api, subscriptions),
        controller_(bind(std::move(options), transport_, api)) {}

  CrowdyStudioAgentController& controller() { return controller_; }
  const CrowdyStudioAgentController& controller() const {
    return controller_;
  }
  std::size_t poll() { return controller_.poll(); }

 private:
  static CrowdyStudioAgentControllerOptions bind(
      CrowdyStudioAgentControllerOptions options,
      CrowdyStudioAgentGraphQLTransport& transport,
      domains::CrowdyStudioAgentAPI& api) {
    options.transport = &transport;
    options.subscriptionAdapter = &transport;
    if (!options.dispatcher) options.dispatcher = api.dispatcher();
    return options;
  }

  CrowdyStudioAgentGraphQLTransport transport_;
  CrowdyStudioAgentController controller_;
};

}  // namespace crowdy::agent
