#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/domains/exec.hpp"
#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/player_wallet.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/models.hpp"

namespace crowdy::studio {

enum class CrowdyStudioDeployment { Draft, Live };

struct CrowdyStudioDeploymentPlan {
  std::string expectedRevisionId;
  std::vector<CrowdyStudioTarget> targets;
  std::optional<CrowdyStudioPairingPreference> pairingPreference;
  std::optional<std::string> projectContentHash;
};

/// What a deploy names. A CLIENT compile resolves the source from the project
/// itself — its saved files at the current revision, or the rust at
/// `commitSha` for a project bound to GitHub — so no file bodies travel with
/// it. The SERVER target is a ck-exec mod, built from `files`.
struct CrowdyStudioDeployTargetInput {
  CrowdyStudioProjectScope scope;
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string moduleName;
  std::string projectId;
  /// The bound project's mirror commit (`project.github->sha`); empty for a
  /// Studio project.
  std::optional<std::string> commitSha;
  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
  /// The target's project files.
  std::vector<CrowdyStudioProjectFile> files;
};

struct CrowdyStudioDeploySubmission {
  std::string versionId;
};

struct CrowdyStudioRuntimeVersion {
  std::string versionId;
  std::string compileStatus;
  std::optional<std::string> compileLog;
};

struct CrowdyStudioClientArtifact {
  std::string versionId;
  std::string artifactHash;
  std::vector<std::uint8_t> bytes;
  std::string fuelPerDispatch;
  std::optional<std::string> contractJson;
};

/// A mod endpoint's decoded reply as JSON, and the call's round trip.
struct CrowdyStudioInvokeResult {
  std::string resultJson;
  std::int64_t durationUs = 0;
};

/// One line the SERVER target's mod logged (`ctx.log`).
struct CrowdyStudioLogLine {
  std::string id;
  std::string moduleName;
  /// "error", "warn", "info" or "debug".
  std::string level;
  std::string at;
  std::string text;
};

struct CrowdyStudioUsageSnapshot {
  std::string hourUnitsUsed;
  std::string dayUnitsUsed;
  std::optional<std::string> unitsPerHour;
  std::optional<std::string> unitsPerDay;
  int compilesThisHour = 0;
  int maxCompilesPerHour = 0;
  std::string gateStatus;
  std::optional<std::string> gateReason;
};

struct CrowdyStudioWalletSnapshot {
  /// Micro-USD (1 USD = 1,000,000) as a decimal string; the unit of account
  /// since the lossless ledger (ck-api 2026-09-11). Spendable = balance - holds.
  std::string balanceMicrousd;
  std::string holdsMicrousd;
  /// Deprecated: balanceMicrousd / 10,000 truncated toward zero.
  std::string balanceCents;
  std::string currency;

  bool operator==(const CrowdyStudioWalletSnapshot&) const = default;
};

/// Optional, viewer-scoped wallet observation seam. It deliberately exposes
/// only the caller's current balance and cannot spend, recharge, mutate billing
/// policy, or act for another user.
class ICrowdyStudioWalletProvider {
 public:
  virtual ~ICrowdyStudioWalletProvider() = default;
  virtual CrowdyStudioWalletSnapshot balance() = 0;
};

/// Engine-owned execution of an already authorized, exact CLIENT artifact.
/// Rendering, host-call routing, and sandbox lifecycle remain outside CrowdyCPP.
class ICrowdyStudioClientRuntime {
 public:
  virtual ~ICrowdyStudioClientRuntime() = default;
  virtual void start(const CrowdyStudioClientArtifact& artifact) = 0;
  virtual void stop() = 0;
};

/// Injectable runtime seam used by the headless controller: the SERVER target
/// as a ck-exec mod, the CLIENT target through player compute. Fakes can
/// implement this directly; CrowdyStudioModRuntime is the production adapter
/// over CrowdyClient::exec() and CrowdyClient::playerCompute().
class ICrowdyStudioRuntime {
 public:
  virtual ~ICrowdyStudioRuntime() = default;

  virtual CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) = 0;
  virtual std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName) = 0;
  virtual void setEnabled(const CrowdyStudioProjectScope& scope,
                          std::string_view moduleName, bool enabled) = 0;
  virtual void startClient(const CrowdyStudioProjectScope& scope,
                           std::string_view moduleName,
                           std::string_view versionId) = 0;
  virtual void stopClient() = 0;
  virtual CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName,
      std::string_view method,
      const std::optional<std::string>& paramsJson) = 0;

  virtual std::vector<CrowdyStudioLogLine> logs(
      const CrowdyStudioProjectScope&, std::string_view) {
    return {};
  }
  virtual std::optional<CrowdyStudioUsageSnapshot> usage(
      std::string_view) {
    return std::nullopt;
  }
};

struct CrowdyStudioLiveApprovalRequest {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  std::string projectRevisionId;
  std::string projectContentHash;
  std::vector<CrowdyStudioTarget> targets;
  CrowdyStudioPairingPreference pairingPreference =
      CrowdyStudioPairingPreference::None;
};

struct CrowdyStudioRestoreApprovalRequest {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  std::string checkpointId;
  std::string expectedRevisionId;
};

/// Approval authority is deliberately injected from the durable agent layer.
/// The Studio controller cannot mint, weaken, or infer an approval.
class ICrowdyStudioApprovalGate {
 public:
  virtual ~ICrowdyStudioApprovalGate() = default;
  virtual void requireLiveApproval(
      const CrowdyStudioLiveApprovalRequest& request,
      std::string_view approvalGrant) = 0;
  virtual void requireRestoreApproval(
      const CrowdyStudioRestoreApprovalRequest& request,
      std::string_view approvalGrant) = 0;
};

/// Minimal production adapter over the public viewer-scoped PlayerWalletAPI
/// balance read. No billing/provider authority crosses this interface.
class CrowdyStudioPlayerWalletProvider final
    : public ICrowdyStudioWalletProvider {
 public:
  explicit CrowdyStudioPlayerWalletProvider(
      domains::PlayerWalletAPI& playerWallet)
      : playerWallet_(&playerWallet) {}

  explicit CrowdyStudioPlayerWalletProvider(
      std::shared_ptr<domains::PlayerWalletAPI> playerWallet)
      : playerWalletOwner_(std::move(playerWallet)),
        playerWallet_(playerWalletOwner_.get()) {
    if (!playerWallet_) {
      throw std::invalid_argument(
          "Crowdy Studio wallet provider requires PlayerWalletAPI");
    }
  }

  CrowdyStudioWalletSnapshot balance() override {
    const graphql::Json value = playerWallet_->balance();
    CrowdyStudioWalletSnapshot snapshot;
    snapshot.balanceMicrousd = scalarString(value["balanceMicrousd"]);
    snapshot.holdsMicrousd = scalarString(value["holdsMicrousd"]);
    snapshot.balanceCents = scalarString(value["balanceCents"]);
    snapshot.currency = value["currency"].asString();
    if (snapshot.balanceMicrousd.empty() || snapshot.currency.empty()) {
      throw std::runtime_error(
          "Player wallet balance response is incomplete");
    }
    return snapshot;
  }

 private:
  static std::string scalarString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    if (value.isString()) return value.asString();
    if (value.isNumber()) return std::to_string(value.asInt64());
    return {};
  }

  std::shared_ptr<domains::PlayerWalletAPI> playerWalletOwner_;
  domains::PlayerWalletAPI* playerWallet_ = nullptr;
};

/// The production runtime: the SERVER target is the grid's ck-exec mod
/// (`mod:<name>`, keyed by the grid), built from the target's crate files with
/// modBuild, deployed when the build succeeds and switched with
/// modSetEnabled; Invoke calls one of its endpoints over an exec connection
/// and Logs are its `ctx.log` lines. The CLIENT target compiles through player
/// compute and runs in the engine-owned client runtime.
///
/// `pump` runs while a mod call waits for its reply: pass what drains the
/// client's dispatcher (CrowdyClient::poll), since exec callbacks run there.
class CrowdyStudioModRuntime final : public ICrowdyStudioRuntime {
 public:
  CrowdyStudioModRuntime(domains::ExecAPI& exec,
                         domains::PlayerComputeAPI& playerCompute,
                         ICrowdyStudioClientRuntime* clientRuntime = nullptr,
                         std::function<void()> pump = {})
      : exec_(exec),
        playerCompute_(playerCompute),
        clientRuntime_(clientRuntime),
        pump_(std::move(pump)) {}

  CrowdyStudioModRuntime(std::shared_ptr<domains::ExecAPI> exec,
                         std::shared_ptr<domains::PlayerComputeAPI> playerCompute,
                         std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntime = {},
                         std::function<void()> pump = {})
      : execOwner_(std::move(exec)),
        playerComputeOwner_(std::move(playerCompute)),
        clientRuntimeOwner_(std::move(clientRuntime)),
        exec_(require(execOwner_, "ExecAPI")),
        playerCompute_(require(playerComputeOwner_, "PlayerComputeAPI")),
        clientRuntime_(clientRuntimeOwner_.get()),
        pump_(std::move(pump)) {}

  ~CrowdyStudioModRuntime() override {
    for (auto& [name, connection] : connections_) {
      if (connection) connection->close();
    }
  }

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) override {
    if (input.target == CrowdyStudioTarget::Server) return buildMod(input);
    if (input.projectId.empty()) {
      throw std::invalid_argument(
          "Crowdy Studio deploy input names no project");
    }
    graphql::JVal variables;
    variables["appId"] = input.scope.appId;
    variables["gridId"] = input.scope.gridId;
    variables["projectId"] = input.projectId;
    variables["name"] = input.moduleName;
    if (input.commitSha && !input.commitSha->empty()) {
      variables["commitSha"] = *input.commitSha;
    }
    variables["draft"] =
        input.deployment == CrowdyStudioDeployment::Draft;
    const graphql::Json response = playerCompute_.deploy(variables);
    return {response["versionId"].asString()};
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    const auto build = modBuilds_.find(std::string(moduleName));
    if (build != modBuilds_.end()) return modBuildStatus(scope, build->first, build->second);
    const graphql::Json response =
        playerCompute_.versions(scope.appId, scope.gridId, moduleName);
    std::vector<CrowdyStudioRuntimeVersion> mapped;
    response.forEach([&](const graphql::Json& value) {
      CrowdyStudioRuntimeVersion version;
      version.versionId = value["versionId"].asString();
      version.compileStatus = value["compileStatus"].asString();
      if (value["compileLog"].ok() && !value["compileLog"].isNull()) {
        version.compileLog = value["compileLog"].asString();
      }
      mapped.push_back(std::move(version));
    });
    return mapped;
  }

  void setEnabled(const CrowdyStudioProjectScope& scope,
                  std::string_view moduleName, bool enabled) override {
    (void)exec_.modSetEnabled(scope.appId, scope.gridId, std::string(moduleName), enabled);
  }

  void startClient(const CrowdyStudioProjectScope& scope,
                   std::string_view moduleName,
                   std::string_view versionId) override {
    if (!clientRuntime_) {
      throw std::runtime_error(
          "CLIENT execution requires an engine-owned artifact runtime");
    }
    const graphql::Json response = playerCompute_.artifact(
        scope.appId, scope.gridId, moduleName, versionId);
    CrowdyStudioClientArtifact artifact;
    artifact.versionId = response["versionId"].asString();
    artifact.artifactHash = response["artifactHash"].asString();
    artifact.fuelPerDispatch =
        scalarString(response["clientFuelPerDispatch"]);
    if (response["contractJson"].ok() &&
        !response["contractJson"].isNull()) {
      artifact.contractJson = response["contractJson"].asString();
    }
    const auto bytes =
        core::base64Decode(response["artifactBase64"].asStringView());
    if (artifact.versionId != versionId || artifact.artifactHash.empty() ||
        !bytes || bytes->empty()) {
      throw std::runtime_error(
          "Client artifact did not match the compiled project version");
    }
    artifact.bytes = *bytes;
    clientRuntime_->start(artifact);
  }

  void stopClient() override {
    if (clientRuntime_) clientRuntime_->stop();
  }

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope& scope, std::string_view moduleName,
      std::string_view method,
      const std::optional<std::string>& paramsJson) override {
    // MessagePack nil: a call with no arguments.
    std::string payload(1, static_cast<char>(0xc0));
    if (paramsJson && paramsJson->find_first_not_of(" \t\r\n") != std::string::npos) {
      const graphql::Json args = graphql::Json::parse(*paramsJson);
      if (!args.ok()) {
        throw std::invalid_argument("The call arguments must be JSON");
      }
      payload = args.toMsgpack();
    }
    const std::string nodeType = domains::execModType(moduleName);
    auto connection = modConnection(scope, moduleName);
    auto reply = std::make_shared<std::promise<domains::ExecReply>>();
    std::future<domains::ExecReply> replied = reply->get_future();
    const auto started = std::chrono::steady_clock::now();
    connection->callRaw(nodeType, scope.gridId, std::string(method), std::move(payload),
                        [reply](domains::ExecReply value) { reply->set_value(std::move(value)); });
    // The connection answers DeadlineExceeded after its own call timeout; this
    // bound only stops a wait whose callbacks nothing is draining.
    const auto giveUp = started + std::chrono::seconds(30);
    while (replied.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) {
      if (pump_) pump_();
      if (std::chrono::steady_clock::now() > giveUp) {
        throw std::runtime_error("DeadlineExceeded: the mod call got no reply");
      }
    }
    const domains::ExecReply value = replied.get();
    if (!value.ok()) {
      throw std::runtime_error(std::string(domains::execStatusName(value.status)) + ": " +
                               value.message());
    }
    CrowdyStudioInvokeResult result;
    const graphql::Json decoded = value.value();
    result.resultJson = decoded.ok() ? decoded.dump() : "null";
    result.durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    return result;
  }

  std::vector<CrowdyStudioLogLine> logs(
      const CrowdyStudioProjectScope& scope,
      std::string_view moduleName) override {
    domains::ExecLogsQuery query;
    query.limit = 50;
    const graphql::Json response =
        exec_.modLogs(scope.appId, scope.gridId, std::string(moduleName), query);
    static constexpr std::string_view kLevels[] = {"error", "warn", "info", "debug"};
    std::vector<CrowdyStudioLogLine> lines;
    response.forEach([&](const graphql::Json& value) {
      CrowdyStudioLogLine line;
      line.id = value["id"].asString();
      line.moduleName = std::string(moduleName);
      const std::int64_t level = value["level"].asInt64(3);
      line.level = std::string(level >= 0 && level < 4 ? kLevels[level] : kLevels[3]);
      line.at = value["at"].asString();
      line.text = value["text"].asString();
      lines.push_back(std::move(line));
    });
    return lines;
  }

  std::optional<CrowdyStudioUsageSnapshot> usage(
      std::string_view appId) override {
    const graphql::Json value = playerCompute_.usage(appId);
    CrowdyStudioUsageSnapshot usage;
    usage.hourUnitsUsed = scalarString(value["hourUnitsUsed"]);
    usage.dayUnitsUsed = scalarString(value["dayUnitsUsed"]);
    usage.unitsPerHour = optionalString(value["unitsPerHour"]);
    usage.unitsPerDay = optionalString(value["unitsPerDay"]);
    usage.compilesThisHour =
        static_cast<int>(value["compilesThisHour"].asInt64());
    usage.maxCompilesPerHour =
        static_cast<int>(value["maxCompilesPerHour"].asInt64());
    usage.gateStatus = value["gateStatus"].asString();
    usage.gateReason = optionalString(value["gateReason"]);
    return usage;
  }

 private:
  struct ModBuild {
    std::string buildId;
    bool deployed = false;
  };

  CrowdyStudioDeploySubmission buildMod(const CrowdyStudioDeployTargetInput& input) {
    const std::string& name = input.moduleName;
    if (!isModName(name)) {
      throw std::invalid_argument(
          "The server module name '" + name +
          "' must be 1-48 lowercase letters, digits, - or _ to run as a mod");
    }
    domains::ExecCrate crate;
    // A build's crate names need a leading letter; a mod's name need not.
    crate.name = name.front() >= 'a' && name.front() <= 'z' ? name : "mod-" + name;
    for (const auto& file : input.files) {
      // What a mod build takes: Cargo.toml, README.md and Rust under src/.
      const std::string& path = file.path;
      const bool rust = path.rfind("src/", 0) == 0 && path.size() > 3 &&
                        path.compare(path.size() - 3, 3, ".rs") == 0;
      if (path == "Cargo.toml" || path == "README.md" || rust) {
        crate.files.emplace_back(path, file.content);
      }
    }
    const graphql::Json queued = exec_.modBuild(input.scope.appId, crate);
    const std::string buildId = queued["buildId"].asString();
    if (buildId.empty()) throw std::runtime_error("execModBuild returned no build id");
    modBuilds_[name] = {buildId, false};
    return {buildId};
  }

  std::vector<CrowdyStudioRuntimeVersion> modBuildStatus(const CrowdyStudioProjectScope& scope,
                                                         const std::string& name, ModBuild& build) {
    const graphql::Json status = exec_.modBuildStatus(scope.appId, build.buildId);
    CrowdyStudioRuntimeVersion version;
    version.versionId = build.buildId;
    version.compileStatus = status["status"].asString();
    if (status["log"].ok() && !status["log"].isNull()) version.compileLog = status["log"].asString();
    if (version.compileStatus == "succeeded" && !build.deployed) {
      // A new mod starts switched off; the controller enables it next.
      (void)exec_.modDeploy(scope.appId, scope.gridId, name, build.buildId);
      build.deployed = true;
    }
    return {version};
  }

  std::shared_ptr<domains::ExecConnection> modConnection(const CrowdyStudioProjectScope& scope,
                                                         std::string_view moduleName) {
    const std::string key = scope.appId + "/" + scope.gridId + "/" + std::string(moduleName);
    auto& connection = connections_[key];
    if (!connection) {
      domains::ExecConnectOptions options;
      options.nodeType = domains::execModType(moduleName);
      options.key = scope.gridId;
      connection = exec_.connect(scope.appId, std::move(options));
    }
    return connection;
  }

  /// As ck-exec's mod names: 1-48 lowercase letters, digits, - or _.
  static bool isModName(std::string_view name) {
    if (name.empty() || name.size() > 48) return false;
    for (const char c : name) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      if (!ok) return false;
    }
    return true;
  }

  template <typename T>
  static T& require(const std::shared_ptr<T>& value, const char* what) {
    if (!value) {
      throw std::invalid_argument(std::string("Crowdy Studio runtime requires ") + what);
    }
    return *value;
  }

  static std::string scalarString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    if (value.isString()) return value.asString();
    return std::to_string(value.asInt64());
  }

  static std::optional<std::string> optionalString(
      const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return std::nullopt;
    return scalarString(value);
  }

  std::shared_ptr<domains::ExecAPI> execOwner_;
  std::shared_ptr<domains::PlayerComputeAPI> playerComputeOwner_;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntimeOwner_;
  domains::ExecAPI& exec_;
  domains::PlayerComputeAPI& playerCompute_;
  ICrowdyStudioClientRuntime* clientRuntime_;
  std::function<void()> pump_;
  std::map<std::string, ModBuild> modBuilds_;
  std::map<std::string, std::shared_ptr<domains::ExecConnection>> connections_;
};

}  // namespace crowdy::studio
