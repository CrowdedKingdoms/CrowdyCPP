#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.playerCompute() — player-authored Rust/WASM bound to player-owned
/// grids. Deploying requires current ownership plus target-specific write
/// permission at both app-tier and grid ACL layers. Enabling separately
/// requires run permission, successful compilation, and admission when the app
/// uses strict allow-list mode. Targets the Game API.
namespace crowdy::domains {

class PlayerComputeAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Create/update a grid-bound module and publish an immutable pending source
  /// version. Compilation is asynchronous.
  graphql::Json deploy(const graphql::JVal& input) const {
    return byInput("PlayerComputeDeploy", input);
  }
  void deployAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerComputeDeploy", input, std::move(cb));
  }

  /// Request activation or stop execution. Enabling checks ownership, target
  /// run permission, compile success, and app admission.
  graphql::Json setEnabled(std::string_view appId, std::string_view gridId,
                           std::string_view name, bool enabled) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    return run("PlayerComputeSetEnabled", vars);
  }
  void setEnabledAsync(std::string_view appId, std::string_view gridId,
                       std::string_view name, bool enabled,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    runAsync("PlayerComputeSetEnabled", vars, std::move(cb));
  }

  /// List modules authored by the caller or installed on grids they currently
  /// own. Closed source is not returned by this module-level operation.
  graphql::Json myModules(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerComputeMyModules", vars);
  }
  void myModulesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerComputeMyModules", vars, std::move(cb));
  }

  /// List immutable versions newest-first. Source and compile logs are
  /// redacted unless the caller is the personal author or source is open.
  graphql::Json versions(std::string_view appId, std::string_view gridId,
                         std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    return run("PlayerComputeVersions", vars);
  }
  void versionsAsync(std::string_view appId, std::string_view gridId,
                     std::string_view name, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    runAsync("PlayerComputeVersions", vars, std::move(cb));
  }

  /// Delete a self-authored module and its versions. The caller must still own
  /// the grid. Returns false when no matching module exists.
  graphql::Json remove(std::string_view appId, std::string_view gridId,
                       std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    return run("PlayerComputeDelete", vars);
  }
  void removeAsync(std::string_view appId, std::string_view gridId,
                   std::string_view name, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    runAsync("PlayerComputeDelete", vars, std::move(cb));
  }

  /// Synchronously invoke an enabled/admitted server module as the grid owner.
  graphql::Json invoke(std::string_view appId, std::string_view gridId,
                       std::string_view moduleName,
                       std::string_view exportName,
                       std::string_view paramsJson = "{}") const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["moduleName"] = moduleName;
    vars["exportName"] = exportName;
    vars["paramsJson"] = paramsJson;
    return run("PlayerComputeInvoke", vars);
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::playerCompute::kPlayerComputeDocument, vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars,
                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::playerCompute::kPlayerComputeDocument, vars, op,
                    std::move(cb));
  }
  graphql::Json byInput(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run(op, vars);
  }
  void byInputAsync(std::string_view op, const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync(op, vars, std::move(cb));
  }
};

}  // namespace crowdy::domains
