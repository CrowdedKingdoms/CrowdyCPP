#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// Operator platform-policy surface — client.operator_(). As of the unified
/// galaxy API (v13) this is reduced to the platform-wide compute ceilings:
/// dedicated customer environments were retired, and the infrastructure
/// control plane (environments, change orders, secrets, release management,
/// audit) moved to the separate infra-control-plane service with its own auth
/// and operator console. The surviving operations still require is_operator;
/// the server enforces the flag on every call.
namespace crowdy::domains {

class OperatorAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Platform-wide compute ceilings (the maxima computeSetPolicy clamps to).
  /// Nullable knobs: null = no operator override (game-api bootstrap default).
  graphql::Json computePlatformCeilings() const {
    return run("CpComputePlatformCeilings", graphql::JVal());
  }
  void computePlatformCeilingsAsync(graphql::GraphQLCallback cb) const {
    runAsync("CpComputePlatformCeilings", graphql::JVal(), std::move(cb));
  }
  /// Patch the compute ceilings. Per knob: omit = unchanged, explicit null =
  /// clear the override, positive value = set. Replica-syncs to game-api
  /// (no restart, <=30s cache bound) and writes an audit entry.
  graphql::Json setComputePlatformCeilings(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("CpSetComputePlatformCeilings", vars);
  }
  void setComputePlatformCeilingsAsync(const graphql::JVal& input,
                                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync("CpSetComputePlatformCeilings", vars, std::move(cb));
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::controlPlane::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::controlPlane::documentFor(op), vars, op, std::move(cb));
  }
};

}  // namespace crowdy::domains
