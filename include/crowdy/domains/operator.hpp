#pragma once

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
  graphql::Json environment(std::string_view slug) const {
    graphql::JVal vars;
    vars["slug"] = slug;
    return run("CpEnvironment", vars);
  }
  graphql::Json changeOrders(const graphql::JVal& vars = graphql::JVal()) const {
    return run("CpChangeOrders", vars);
  }
  graphql::Json changeOrder(std::string_view id) const {
    graphql::JVal vars;
    vars["id"] = id;
    return run("CpChangeOrder", vars);
  }
  graphql::Json audit(const graphql::JVal& vars = graphql::JVal()) const {
    return run("CpAudit", vars);
  }
  graphql::Json secrets(std::string_view environmentId = {}) const {
    graphql::JVal vars;
    if (!environmentId.empty()) vars["environmentId"] = environmentId;
    return run("CpSecrets", vars);
  }
  graphql::Json envSecrets(std::string_view environmentId = {}) const {
    graphql::JVal vars;
    if (!environmentId.empty()) vars["environmentId"] = environmentId;
    return run("CpEnvSecrets", vars);
  }
  graphql::Json ovhCatalogSummary(std::string_view region = {}) const {
    graphql::JVal vars;
    if (!region.empty()) vars["region"] = region;
    return run("CpOvhCatalogSummary", vars);
  }
  graphql::Json usageSummary(std::string_view environmentSlug, std::string_view sinceIso) const {
    graphql::JVal vars;
    vars["environmentSlug"] = environmentSlug;
    vars["since"] = sinceIso;
    return run("CpUsageSummary", vars);
  }
  graphql::Json unreleasedGameApiTags() const {
    return run("CpUnreleasedGameApiTags", graphql::JVal());
  }
  graphql::Json environmentVersions() const {
    return run("CpEnvironmentVersions", graphql::JVal());
  }
  graphql::Json operatorUsers() const { return run("OperatorUsers", graphql::JVal()); }

  graphql::Json setEnvironmentDeletionProtection(const graphql::JVal& vars) const {
    return run("SetEnvironmentDeletionProtection", vars);
  }
  graphql::Json putSecret(const graphql::JVal& vars) const { return run("PutCpSecret", vars); }
  graphql::Json deleteSecret(std::string_view environmentId, std::string_view name) const {
    graphql::JVal vars;
    vars["environmentId"] = environmentId;
    vars["name"] = name;
    return run("DeleteCpSecret", vars);
  }
  graphql::Json putEnvSecret(const graphql::JVal& vars) const {
    return run("PutCpEnvSecret", vars);
  }
  graphql::Json ingestEnvironmentVersion(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("IngestEnvironmentVersion", vars);
  }
  graphql::Json publishReleaseFromGameApiTag(const graphql::JVal& vars) const {
    return run("PublishEnvironmentReleaseFromGameApiTag", vars);
  }
  graphql::Json yankEnvironmentVersion(std::string_view version) const {
    graphql::JVal vars;
    vars["version"] = version;
    return run("YankEnvironmentVersion", vars);
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::controlPlane::kControlPlaneDocument, vars, op);
  }
};

}  // namespace crowdy::domains
