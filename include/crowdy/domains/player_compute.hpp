#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// client.playerCompute() — players' CLIENT modules: Rust compiled on the
/// platform to WASM for a native sandbox, bound to player-owned grids.
/// Deploying requires current ownership plus write_client_code at both
/// app-tier and grid ACL layers; fetching the artifact requires
/// run_client_code and admission when the app uses strict allow-list mode.
/// Server-side player code is a ck-exec mod (client.exec().mod*).
namespace crowdy::domains {

class PlayerComputeAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;
  using ArtifactBytesCallback = std::function<void(
      graphql::GraphQLOutcome, ClientArtifactBytes)>;

  /// Compile a project's CLIENT target into an immutable pending version of a
  /// grid-bound module (`target` is set to CLIENT). Compilation is
  /// asynchronous; poll versions().
  graphql::Json deploy(const graphql::JVal& input) const {
    return byInput("PlayerComputeDeploy", clientTarget(input));
  }
  void deployAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerComputeDeploy", clientTarget(input), std::move(cb));
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

  /// The caller's compile quota for one app and the wallet/spend-cap gate
  /// state with its typed reason.
  graphql::Json usage(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerComputeUsage", vars);
  }
  void usageAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerComputeUsage", vars, std::move(cb));
  }

  /// Throw or release a kill-ladder switch at player/grid/app/listing scope
  /// (studio, requires manage_compute); a thrown switch stops artifact
  /// fetches. Pass listingRef in options for LISTING scope.
  graphql::Json setSwitch(std::string_view appId, std::string_view scope,
                          bool disabled,
                          const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["scope"] = scope;
    vars["disabled"] = disabled;
    return run("PlayerComputeSetSwitch", vars);
  }
  void setSwitchAsync(std::string_view appId, std::string_view scope,
                      bool disabled, const graphql::JVal& options,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["scope"] = scope;
    vars["disabled"] = disabled;
    runAsync("PlayerComputeSetSwitch", vars, std::move(cb));
  }

  /// Active kill-ladder switches (studio, requires view_compute_diagnostics).
  graphql::Json switches(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerComputeSwitches", vars);
  }
  void switchesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerComputeSwitches", vars, std::move(cb));
  }

  /// Fetch a compiled CLIENT artifact + metadata (P3). Fail-closed server-side
  /// (ownership, authorship, run_client_code, admission). Native clients get
  /// the metadata + gas-injected bytes; the browser broker itself is
  /// JS-only (04 §7), so CrowdyCPP wraps the fetch surface, not a sandbox.
  graphql::Json artifact(std::string_view appId, std::string_view gridId,
                         std::string_view name,
                         std::string_view versionId = "") const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    if (!versionId.empty()) vars["versionId"] = versionId;
    return run("PlayerComputeArtifact", vars);
  }
  void artifactAsync(std::string_view appId, std::string_view gridId,
                     std::string_view name, std::string_view versionId,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    vars["name"] = name;
    if (!versionId.empty()) vars["versionId"] = versionId;
    runAsync("PlayerComputeArtifact", vars, std::move(cb));
  }

  /// Fetch and base64-decode a CLIENT artifact for a native sandbox/runtime.
  /// fuelPerDispatch remains a decimal string to preserve GraphQL BigInt.
  ClientArtifactBytes artifactBytes(
      std::string_view appId, std::string_view gridId,
      std::string_view name, std::string_view versionId = "") const {
    return requireArtifactBytes(artifact(appId, gridId, name, versionId));
  }
  void artifactBytesAsync(
      std::string_view appId, std::string_view gridId,
      std::string_view name, std::string_view versionId,
      ArtifactBytesCallback cb) const {
    artifactAsync(
        appId, gridId, name, versionId,
        [cb = std::move(cb)](graphql::GraphQLOutcome outcome) mutable {
          ClientArtifactBytes decoded;
          if (outcome.ok()) {
            auto value = decodeClientArtifactBytes(outcome.data);
            if (value) {
              decoded = std::move(*value);
            } else {
              outcome.status = Errc::Malformed;
              outcome.kind = graphql::GraphQLErrorKind::Protocol;
              outcome.errorMessage =
                  "playerComputeArtifact returned invalid artifact bytes";
            }
          }
          cb(std::move(outcome), std::move(decoded));
        });
  }
  void artifactBytesAsync(
      std::string_view appId, std::string_view gridId,
      std::string_view name, ArtifactBytesCallback cb) const {
    artifactBytesAsync(appId, gridId, name, {}, std::move(cb));
  }

 private:
  static graphql::JVal clientTarget(const graphql::JVal& input) {
    graphql::JVal in = input;
    in["target"] = "CLIENT";
    return in;
  }

  static ClientArtifactBytes requireArtifactBytes(
      const graphql::Json& artifact) {
    auto decoded = decodeClientArtifactBytes(artifact);
    if (decoded) return std::move(*decoded);
#ifndef CROWDY_NO_EXCEPTIONS
    throw graphql::CrowdyProtocolError(
        "playerComputeArtifact returned invalid artifact bytes");
#else
    return {};
#endif
  }

  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::playerCompute::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars,
                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::playerCompute::documentFor(op), vars, op,
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
