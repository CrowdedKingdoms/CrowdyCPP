#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// Operator control-plane surface — client.operator_(). Platform operations
/// that require is_operator: cross-org environments, change orders, secrets,
/// release management, and audit. For internal operator tooling only; the
/// server enforces the flag on every call. Targets the Management API.
namespace crowdy::domains {

class OperatorAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json environments(int page = 1, int pageSize = 50) const {
    graphql::JVal vars;
    vars["page"] = std::int64_t{page};
    vars["pageSize"] = std::int64_t{pageSize};
    return run("CpEnvironments", vars);
  }
  void environmentsAsync(int page, int pageSize, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["page"] = std::int64_t{page};
    vars["pageSize"] = std::int64_t{pageSize};
    runAsync("CpEnvironments", vars, std::move(cb));
  }
  graphql::Json environment(std::string_view slug) const {
    graphql::JVal vars;
    vars["slug"] = slug;
    return run("CpEnvironment", vars);
  }
  void environmentAsync(std::string_view slug, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["slug"] = slug;
    runAsync("CpEnvironment", vars, std::move(cb));
  }
  graphql::Json changeOrders(const graphql::JVal& vars = graphql::JVal()) const {
    return run("CpChangeOrders", vars);
  }
  void changeOrdersAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    runAsync("CpChangeOrders", vars, std::move(cb));
  }
  graphql::Json changeOrder(std::string_view id) const {
    graphql::JVal vars;
    vars["id"] = id;
    return run("CpChangeOrder", vars);
  }
  void changeOrderAsync(std::string_view id, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["id"] = id;
    runAsync("CpChangeOrder", vars, std::move(cb));
  }
  graphql::Json audit(const graphql::JVal& vars = graphql::JVal()) const {
    return run("CpAudit", vars);
  }
  void auditAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    runAsync("CpAudit", vars, std::move(cb));
  }
  graphql::Json secrets(std::string_view environmentId = {}) const {
    graphql::JVal vars;
    if (!environmentId.empty()) vars["environmentId"] = environmentId;
    return run("CpSecrets", vars);
  }
  void secretsAsync(std::string_view environmentId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    if (!environmentId.empty()) vars["environmentId"] = environmentId;
    runAsync("CpSecrets", vars, std::move(cb));
  }
  graphql::Json envSecrets(std::string_view environmentId = {}) const {
    graphql::JVal vars;
    if (!environmentId.empty()) vars["environmentId"] = environmentId;
    return run("CpEnvSecrets", vars);
  }
  void envSecretsAsync(std::string_view environmentId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    if (!environmentId.empty()) vars["environmentId"] = environmentId;
    runAsync("CpEnvSecrets", vars, std::move(cb));
  }
  graphql::Json ovhCatalogSummary(std::string_view region = {}) const {
    graphql::JVal vars;
    if (!region.empty()) vars["region"] = region;
    return run("CpOvhCatalogSummary", vars);
  }
  void ovhCatalogSummaryAsync(std::string_view region, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    if (!region.empty()) vars["region"] = region;
    runAsync("CpOvhCatalogSummary", vars, std::move(cb));
  }
  graphql::Json usageSummary(std::string_view environmentSlug, std::string_view sinceIso) const {
    graphql::JVal vars;
    vars["environmentSlug"] = environmentSlug;
    vars["since"] = sinceIso;
    return run("CpUsageSummary", vars);
  }
  void usageSummaryAsync(std::string_view environmentSlug, std::string_view sinceIso,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["environmentSlug"] = environmentSlug;
    vars["since"] = sinceIso;
    runAsync("CpUsageSummary", vars, std::move(cb));
  }
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
  graphql::Json unreleasedGameApiTags() const {
    return run("CpUnreleasedGameApiTags", graphql::JVal());
  }
  void unreleasedGameApiTagsAsync(graphql::GraphQLCallback cb) const {
    runAsync("CpUnreleasedGameApiTags", graphql::JVal(), std::move(cb));
  }
  graphql::Json environmentVersions() const {
    return run("CpEnvironmentVersions", graphql::JVal());
  }
  void environmentVersionsAsync(graphql::GraphQLCallback cb) const {
    runAsync("CpEnvironmentVersions", graphql::JVal(), std::move(cb));
  }
  graphql::Json operatorUsers() const { return run("OperatorUsers", graphql::JVal()); }
  void operatorUsersAsync(graphql::GraphQLCallback cb) const {
    runAsync("OperatorUsers", graphql::JVal(), std::move(cb));
  }

  graphql::Json setEnvironmentDeletionProtection(const graphql::JVal& vars) const {
    return run("SetEnvironmentDeletionProtection", vars);
  }
  void setEnvironmentDeletionProtectionAsync(const graphql::JVal& vars,
                                             graphql::GraphQLCallback cb) const {
    runAsync("SetEnvironmentDeletionProtection", vars, std::move(cb));
  }
  graphql::Json putSecret(const graphql::JVal& vars) const { return run("PutCpSecret", vars); }
  void putSecretAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    runAsync("PutCpSecret", vars, std::move(cb));
  }
  graphql::Json deleteSecret(std::string_view environmentId, std::string_view name) const {
    graphql::JVal vars;
    vars["environmentId"] = environmentId;
    vars["name"] = name;
    return run("DeleteCpSecret", vars);
  }
  void deleteSecretAsync(std::string_view environmentId, std::string_view name,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["environmentId"] = environmentId;
    vars["name"] = name;
    runAsync("DeleteCpSecret", vars, std::move(cb));
  }
  graphql::Json putEnvSecret(const graphql::JVal& vars) const {
    return run("PutCpEnvSecret", vars);
  }
  void putEnvSecretAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    runAsync("PutCpEnvSecret", vars, std::move(cb));
  }
  graphql::Json ingestEnvironmentVersion(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("IngestEnvironmentVersion", vars);
  }
  void ingestEnvironmentVersionAsync(const graphql::JVal& input,
                                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync("IngestEnvironmentVersion", vars, std::move(cb));
  }
  graphql::Json publishReleaseFromGameApiTag(const graphql::JVal& vars) const {
    return run("PublishEnvironmentReleaseFromGameApiTag", vars);
  }
  void publishReleaseFromGameApiTagAsync(const graphql::JVal& vars,
                                         graphql::GraphQLCallback cb) const {
    runAsync("PublishEnvironmentReleaseFromGameApiTag", vars, std::move(cb));
  }
  graphql::Json yankEnvironmentVersion(std::string_view version) const {
    graphql::JVal vars;
    vars["version"] = version;
    return run("YankEnvironmentVersion", vars);
  }
  void yankEnvironmentVersionAsync(std::string_view version, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["version"] = version;
    runAsync("YankEnvironmentVersion", vars, std::move(cb));
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::controlPlane::kControlPlaneDocument, vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::controlPlane::kControlPlaneDocument, vars, op, std::move(cb));
  }
};

}  // namespace crowdy::domains
