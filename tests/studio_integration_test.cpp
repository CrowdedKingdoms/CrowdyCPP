#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/agent/schema.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/studio/integration.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::agent;
using namespace crowdy::player_host;
using namespace crowdy::studio;

namespace {

template <typename T>
concept HasGraphqlClient = requires(T& value) {
  value.graphqlClient();
};

template <typename T>
concept HasFilesystem = requires(T& value) {
  value.filesystem();
};

template <typename T>
concept HasShell = requires(T& value) {
  value.shell();
};

static_assert(!HasGraphqlClient<ICrowdyStudioEditorAdapter>);
static_assert(!HasGraphqlClient<CrowdyStudioControllerHostAdapter>);
static_assert(!HasGraphqlClient<CrowdyStudioIntegration>);
static_assert(!HasFilesystem<ICrowdyStudioEditorAdapter>);
static_assert(!HasFilesystem<CrowdyStudioControllerHostAdapter>);
static_assert(!HasShell<ICrowdyStudioEditorAdapter>);
static_assert(!HasShell<CrowdyStudioControllerHostAdapter>);

class FakeCrypto final : public core::ICrypto {
 public:
  bool hmacSha256(Bytes key, Bytes message,
                  std::uint8_t* out) const override {
    return hash(key, message, out);
  }

  bool sha256(Bytes message, std::uint8_t* out) const override {
    return hash({}, message, out);
  }

  bool constantTimeEquals(const std::uint8_t* left,
                          const std::uint8_t* right,
                          std::size_t size) const override {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < size; ++index) {
      difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
  }

  bool randomBytes(std::uint8_t* out, std::size_t size) const override {
    for (std::size_t index = 0; index < size; ++index) {
      out[index] = static_cast<std::uint8_t>(index + 1);
    }
    return true;
  }

 private:
  static bool hash(Bytes first, Bytes second, std::uint8_t* out) {
    std::uint32_t value = 2'166'136'261U;
    for (const auto byte : first) {
      value = (value ^ byte) * 16'777'619U;
    }
    for (const auto byte : second) {
      value = (value ^ byte) * 16'777'619U;
    }
    for (std::size_t index = 0; index < 32; ++index) {
      value = value * 1'664'525U + 1'013'904'223U;
      out[index] =
          static_cast<std::uint8_t>((value >> ((index % 4) * 8)) & 0xffU);
    }
    return true;
  }
};

class FixtureCrypto final : public core::ICrypto {
 public:
  bool hmacSha256(Bytes key, Bytes message,
                  std::uint8_t* out) const override {
    std::string input(
        reinterpret_cast<const char*>(key.data()), key.size());
    input.append(
        reinterpret_cast<const char*>(message.data()), message.size());
    return digest(input, out);
  }

  bool sha256(Bytes message, std::uint8_t* out) const override {
    return digest(
        std::string_view(
            reinterpret_cast<const char*>(message.data()),
            message.size()),
        out);
  }

  bool constantTimeEquals(const std::uint8_t* left,
                          const std::uint8_t* right,
                          std::size_t size) const override {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < size; ++index) {
      difference |=
          static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
  }

  bool randomBytes(std::uint8_t* out, std::size_t size) const override {
    for (std::size_t index = 0; index < size; ++index) {
      out[index] = static_cast<std::uint8_t>(index + 1);
    }
    return true;
  }

 private:
  static bool digest(std::string_view value, std::uint8_t* out) {
    const std::string encoded = agent::sha256Digest(value);
    for (std::size_t index = 0; index < 32; ++index) {
      const auto hex = encoded.substr(7 + index * 2, 2);
      out[index] = static_cast<std::uint8_t>(
          std::stoul(hex, nullptr, 16));
    }
    return true;
  }
};

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = 1'784'894'400'000LL;
  std::int64_t monotonic = 0;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
};

CrowdyStudioProjectFile file(CrowdyStudioTarget target, std::string path,
                             std::string content) {
  CrowdyStudioProjectFile value;
  value.target = target;
  value.path = std::move(path);
  value.content = std::move(content);
  value.revision = "1";
  value.createdAt = "2026-07-24T00:00:00.000Z";
  value.updatedAt = value.createdAt;
  return value;
}

CrowdyStudioProject project(std::string id, std::string revision) {
  CrowdyStudioProject value;
  value.projectId = std::move(id);
  value.appId = "42";
  value.ownerUserId = "7";
  value.gridId = "500";
  value.kind = CrowdyStudioProjectKind::Server;
  value.metadata.name =
      value.projectId == "project-1" ? "Fixture Studio"
                                      : "Second project";
  if (value.projectId == "project-1") {
    value.metadata.description = "Cross-SDK projection fixture";
  }
  value.metadata.serverModuleName =
      value.projectId == "project-1" ? "fixture-server"
                                      : "second-server";
  value.files = {
      file(CrowdyStudioTarget::Server, "Cargo.toml",
           "[package]\nname = \"fixture\"\n"),
      file(CrowdyStudioTarget::Server, "src/lib.rs", "pub fn invoke() {}"),
  };
  value.sdkVersion = "0.1.5";
  value.revision = {std::move(revision), "2026-07-24T00:00:00.000Z"};
  value.fileCount = 2;
  value.totalBytes = "45";
  value.createdAt = "2026-07-24T00:00:00.000Z";
  value.updatedAt =
      value.projectId == "project-1"
          ? "2026-07-24T00:00:00.000Z"
          : "2026-07-24T00:00:01.000Z";
  return value;
}

CrowdyStudioProjectSummary summary(const CrowdyStudioProject& value) {
  return {
      value.projectId,
      value.gridId,
      value.metadata.name,
      value.kind,
      value.revision.id,
      value.metadata.serverModuleName,
      value.metadata.clientModuleName,
      value.archived,
      value.updatedAt,
  };
}

class FakeProjectProvider final : public ICrowdyStudioProjectProvider {
 public:
  explicit FakeProjectProvider(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {
    projects = {project("project-1", "1"), project("project-2", "2")};
  }
  ~FakeProjectProvider() override {
    if (events) events->push_back("provider-destroy");
  }

  std::shared_ptr<std::vector<std::string>> events;
  std::vector<CrowdyStudioProject> projects;
  int saves = 0;

  std::vector<CrowdyStudioProjectSummary> listProjects(
      const CrowdyStudioProjectScope&) override {
    std::vector<CrowdyStudioProjectSummary> values;
    for (const auto& value : projects) values.push_back(summary(value));
    return values;
  }

  CrowdyStudioProject getProject(
      const CrowdyStudioProjectScope&,
      std::string_view projectId) override {
    return get(projectId);
  }

  CrowdyStudioProject createProject(
      const CreateCrowdyStudioProjectInput&) override {
    return projects.front();
  }

  CrowdyStudioProject saveProject(
      const SaveCrowdyStudioProjectInput& input) override {
    auto& value = getMutable(input.projectId);
    CHECK(value.revision.id == input.expectedRevisionId);
    value.metadata = input.metadata;
    value.files = input.files;
    value.revision.id =
        std::to_string(std::stoll(value.revision.id) + 1);
    value.updatedAt = "2026-07-24T00:00:01.000Z";
    ++saves;
    return value;
  }

  std::vector<CrowdyStudioReferenceFile> listPersonalLibraryFiles(
      const CrowdyStudioProjectScope&) override {
    CrowdyStudioReferenceFile value;
    value.id = "library-1";
    value.source = CrowdyStudioReferenceSource::PersonalLibrary;
    value.appId = "42";
    value.title = "Library helper";
    value.target = CrowdyStudioTarget::Server;
    value.path = "src/library.rs";
    value.content = "pub fn library() {}";
    value.revision = "1";
    return {value};
  }

  CrowdyStudioReferenceFile savePersonalLibraryFile(
      const SaveCrowdyStudioLibraryFileInput&) override {
    return listPersonalLibraryFiles({}).front();
  }

  std::vector<CrowdyStudioReferenceFile> listCommonFiles(
      const CrowdyStudioProjectScope&) override {
    CrowdyStudioReferenceFile value;
    value.id = "common-1";
    value.source = CrowdyStudioReferenceSource::Common;
    value.appId = "42";
    value.title = "Common helper";
    value.target = CrowdyStudioTarget::Server;
    value.path = "src/common.rs";
    value.content = "pub fn common() {}";
    value.revision = "1";
    return {value};
  }

  CrowdyStudioProject importReferenceFile(
      const ImportCrowdyStudioReferenceFileInput&) override {
    return projects.front();
  }

 private:
  CrowdyStudioProject get(std::string_view id) const {
    const auto found = std::find_if(
        projects.begin(), projects.end(), [&](const auto& value) {
          return value.projectId == id;
        });
    CHECK(found != projects.end());
    return *found;
  }

  CrowdyStudioProject& getMutable(std::string_view id) {
    const auto found = std::find_if(
        projects.begin(), projects.end(), [&](const auto& value) {
          return value.projectId == id;
        });
    CHECK(found != projects.end());
    return *found;
  }
};

class FakeRuntime final : public ICrowdyStudioRuntime {
 public:
  explicit FakeRuntime(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {}
  ~FakeRuntime() override {
    if (events) events->push_back("runtime-destroy");
  }

  std::shared_ptr<std::vector<std::string>> events;
  std::vector<std::string> calls;
  std::function<void()> onVersions;

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) override {
    calls.push_back(
        input.deployment == CrowdyStudioDeployment::Draft
            ? "deploy-draft"
            : "deploy-live");
    return {input.deployment == CrowdyStudioDeployment::Draft
                ? "draft-v1"
                : "live-v1"};
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope&,
      std::string_view) override {
    calls.push_back("versions");
    if (onVersions) {
      auto callback = std::move(onVersions);
      onVersions = {};
      callback();
    }
    const bool live =
        std::find(calls.begin(), calls.end(), "deploy-live") != calls.end();
    return {{live ? "live-v1" : "draft-v1", "succeeded", std::nullopt}};
  }

  void setEnabled(const CrowdyStudioProjectScope&,
                  std::string_view, bool enabled) override {
    calls.push_back(enabled ? "enable" : "disable");
  }

  void setRequires(
      const CrowdyStudioProjectScope&, std::string_view,
      const std::optional<std::string>&) override {
    calls.push_back("requires");
  }

  void startClient(const CrowdyStudioProjectScope&, std::string_view,
                   std::string_view) override {
    calls.push_back("start-client");
  }

  void stopClient() override {
    calls.push_back("stop-client");
    if (events) events->push_back("runtime-stop");
  }

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope&, std::string_view,
      std::string_view, const std::optional<std::string>& params) override {
    calls.push_back("invoke:" + params.value_or(""));
    return {std::nullopt, R"({"ok":true})", "4", 2};
  }
};

class FakeApproval final : public ICrowdyStudioApprovalGate {
 public:
  int live = 0;
  void requireLiveApproval(
      const CrowdyStudioLiveApprovalRequest& request,
      std::string_view grant) override {
    CHECK(request.projectContentHash.rfind("sha256:", 0) == 0);
    CHECK(grant == "approved");
    ++live;
  }
  void requireRestoreApproval(
      const CrowdyStudioRestoreApprovalRequest&,
      std::string_view) override {}
};

class FakeEditor final : public ICrowdyStudioEditorAdapter {
 public:
  explicit FakeEditor(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {}
  ~FakeEditor() override {
    if (events) events->push_back("editor-destroy");
  }

  std::shared_ptr<std::vector<std::string>> events;
  CrowdyStudioEditorCallbacks callbacks;
  std::vector<CrowdyStudioEditorSnapshot> snapshots;
  int relayouts = 0;
  bool disposed = false;

  CrowdyStudioEditorMode mode() const noexcept override {
    return CrowdyStudioEditorMode::Native;
  }
  void setCallbacks(CrowdyStudioEditorCallbacks value) override {
    callbacks = std::move(value);
  }
  void synchronize(
      const CrowdyStudioEditorSnapshot& snapshot) override {
    snapshots.push_back(snapshot);
  }
  void relayout() override { ++relayouts; }
  void dispose() noexcept override {
    disposed = true;
    if (events) events->push_back("editor-dispose");
  }
};

class FakeClientRuntime final : public ICrowdyStudioClientRuntime {
 public:
  explicit FakeClientRuntime(
      std::shared_ptr<std::vector<std::string>> eventLog)
      : events(std::move(eventLog)) {}
  ~FakeClientRuntime() override {
    events->push_back("client-runtime-destroy");
  }
  void start(const CrowdyStudioClientArtifact&) override {}
  void stop() override {}
  std::shared_ptr<std::vector<std::string>> events;
};

class FakePlayerHost final : public PlayerHostAdapterV1 {
 public:
  void capabilities(CancellationTokenV1,
                    CapabilitiesCallbackV1 callback) override {
    PlayerHostCapabilitiesV1 value;
    value.game_id = "game";
    value.revision = "capability-1";
    value.controlled_entity_id = "player-1";
    value.advertised_at = "2026-07-24T00:00:00.000Z";
    callback(AdapterResultV1<PlayerHostCapabilitiesV1>::success(
        std::move(value)));
  }
  void observe(const ObserveRequestV1&, CancellationTokenV1,
               ObservationCallbackV1 callback) override {
    callback(AdapterResultV1<GameObservationV1>::failure(
        {.code = "AGENT_HOST_UNAVAILABLE",
         .message = "not used",
         .retryable = false,
         .remediation = std::nullopt,
         .field = std::nullopt,
         .required_scope = std::nullopt}));
  }
  void dispatch(const GameCommandV1&, const ValidatedGateV1&,
                CancellationTokenV1,
                CommandCallbackV1 callback) override {
    callback(AdapterResultV1<GameCommandResultV1>::failure(
        {.code = "AGENT_HOST_UNAVAILABLE",
         .message = "not used",
         .retryable = false,
         .remediation = std::nullopt,
         .field = std::nullopt,
         .required_scope = std::nullopt}));
  }
  void clearAgentIntent(PreemptionReasonV1) noexcept override {
    ++clears;
  }
  int clears = 0;
};

class FakeHttpTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse send(
      const graphql::HttpRequest&) override {
    return {503, R"({"errors":[{"message":"offline"}]})"};
  }
};

class FakeLayout final : public ICrowdyStudioIntegrationLayout {
 public:
  explicit FakeLayout(
      std::shared_ptr<std::vector<std::string>> eventLog)
      : events(std::move(eventLog)) {}
  ~FakeLayout() override { events->push_back("layout-destroy"); }
  void relayout() override { ++relayouts; }
  void tick() override { ++ticks; }
  void dispose() noexcept override {
    events->push_back("layout-dispose");
  }
  std::shared_ptr<std::vector<std::string>> events;
  int relayouts = 0;
  int ticks = 0;
};

class FakeControl final : public ICrowdyStudioIntegrationControl {
 public:
  explicit FakeControl(
      std::shared_ptr<std::vector<std::string>> eventLog)
      : events(std::move(eventLog)) {}
  ~FakeControl() override { events->push_back("control-destroy"); }
  void tick() override { ++ticks; }
  void dispose() noexcept override {
    events->push_back("control-dispose");
  }
  std::shared_ptr<std::vector<std::string>> events;
  int ticks = 0;
};

CrowdyStudioControllerOptions controllerOptions(FakeClock& clock) {
  CrowdyStudioControllerOptions options;
  options.appId = "42";
  options.gridId = "500";
  options.initialProjectId = "project-1";
  options.autosaveMs = 5;
  options.compilePollMs = 0;
  options.compilePollLimit = 2;
  options.sleep = [](std::int64_t) {};
  (void)clock;
  return options;
}

ValidatedStudioGateV1 gate(
    std::string context = "context-1",
    std::optional<std::string> approval = std::nullopt) {
  return {
      .session_id = "session-1",
      .run_id = "run-1",
      .tool_call_id = "tool-1",
      .client_epoch = "1",
      .context_version = std::move(context),
      .lease_id = "workspace-lease",
      .approval_grant = std::move(approval),
      .validated_at = "2026-07-24T00:00:00.000Z",
  };
}

AdapterResultV1<StudioNativeToolOutputV1> dispatch(
    CrowdyStudioControllerHostAdapter& host,
    StudioNativeToolKindV1 kind,
    StudioNativeToolRequestV1 request,
    ValidatedStudioGateV1 validated = gate(),
    CancellationTokenV1 cancellation = {}) {
  CancellationSourceV1 live;
  if (cancellation.cancelled()) cancellation = live.token();
  std::optional<AdapterResultV1<StudioNativeToolOutputV1>> result;
  host.dispatch(
      kind, request, validated, cancellation,
      [&](auto value) { result = std::move(value); });
  CHECK(result.has_value());
  return std::move(*result);
}

CrowdyStudioControllerHostAdapterOptions hostOptions(
    FakeEditor* editor = nullptr) {
  CrowdyStudioControllerHostAdapterOptions options;
  options.sessionId =
      [] { return std::optional<std::string>("session-1"); };
  options.clientEpoch =
      [] { return std::optional<std::string>("1"); };
  options.contextVersion = [] { return std::string("context-1"); };
  options.leaseKinds = [] {
    return std::vector<LeaseKindV1>{LeaseKindV1::Workspace,
                                    LeaseKindV1::Play};
  };
  options.hostCapabilityRevision =
      [] { return std::optional<std::string>("capability-1"); };
  options.isLeaseActive = [](std::string_view id, LeaseKindV1 kind) {
    return (kind == LeaseKindV1::Workspace &&
            id == "workspace-lease") ||
           (kind == LeaseKindV1::Play && id == "play-lease");
  };
  options.validateApprovalGrant =
      [](StudioNativeToolKindV1,
         const StudioNativeToolRequestV1&,
         const ValidatedStudioGateV1& value) {
        return value.approval_grant == "approved";
      };
  if (editor) {
    options.localDiagnostics = [editor] {
      (void)editor;
      return std::vector<CrowdyStudioEditorDiagnostic>{};
    };
  }
  return options;
}

std::string studioFixtureText(std::string_view name) {
  const std::string path =
      std::string(CROWDY_PARITY_FIXTURE_DIR) + "/" + std::string(name);
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string_view fixtureTargetName(StudioTargetV1 value) {
  return value == StudioTargetV1::Server ? "SERVER" : "CLIENT";
}

std::string_view fixturePairingName(
    StudioPairingPreferenceV1 value) {
  switch (value) {
    case StudioPairingPreferenceV1::None: return "NONE";
    case StudioPairingPreferenceV1::Optional: return "OPTIONAL";
    case StudioPairingPreferenceV1::Required: return "REQUIRED";
  }
  return "";
}

std::string_view fixtureSourceName(StudioFileSourceV1 value) {
  switch (value) {
    case StudioFileSourceV1::Project: return "PROJECT";
    case StudioFileSourceV1::PersonalLibrary:
      return "PERSONAL_LIBRARY";
    case StudioFileSourceV1::Common: return "COMMON";
  }
  return "";
}

std::string_view fixtureSaveStateName(StudioSaveStateV1 value) {
  switch (value) {
    case StudioSaveStateV1::Saving: return "SAVING";
    case StudioSaveStateV1::Saved: return "SAVED";
    case StudioSaveStateV1::Conflict: return "CONFLICT";
    case StudioSaveStateV1::Offline: return "OFFLINE";
  }
  return "";
}

std::string_view fixtureProjectKindName(StudioProjectKindV1 value) {
  switch (value) {
    case StudioProjectKindV1::Server: return "SERVER";
    case StudioProjectKindV1::Client: return "CLIENT";
    case StudioProjectKindV1::FullStack: return "FULL_STACK";
  }
  return "";
}

std::string_view fixtureRuntimePhaseName(
    StudioRuntimePhaseV1 value) {
  switch (value) {
    case StudioRuntimePhaseV1::Idle: return "IDLE";
    case StudioRuntimePhaseV1::TestingDraft: return "TESTING_DRAFT";
    case StudioRuntimePhaseV1::DeployingLive:
      return "DEPLOYING_LIVE";
    case StudioRuntimePhaseV1::Compiling: return "COMPILING";
    case StudioRuntimePhaseV1::Enabling: return "ENABLING";
    case StudioRuntimePhaseV1::Running: return "RUNNING";
    case StudioRuntimePhaseV1::CompileFailed:
      return "COMPILE_FAILED";
    case StudioRuntimePhaseV1::Stopping: return "STOPPING";
    case StudioRuntimePhaseV1::Stopped: return "STOPPED";
    case StudioRuntimePhaseV1::PartialFailure:
      return "PARTIAL_FAILURE";
    case StudioRuntimePhaseV1::Error: return "ERROR";
  }
  return "";
}

std::string_view fixtureRuntimeSyncName(StudioRuntimeSyncV1 value) {
  switch (value) {
    case StudioRuntimeSyncV1::NeverRun: return "NEVER_RUN";
    case StudioRuntimeSyncV1::RunningSaved:
      return "RUNNING_SAVED";
    case StudioRuntimeSyncV1::RunningStale:
      return "RUNNING_STALE";
    case StudioRuntimeSyncV1::Stopped: return "STOPPED";
  }
  return "";
}

graphql::JVal fixtureRuntimeJson(
    const StudioRuntimeStatusV1& value) {
  graphql::JVal result;
  result["phase"] = fixtureRuntimePhaseName(value.phase);
  result["savedRevision"] = value.saved_revision;
  if (value.running_revision) {
    result["runningRevision"] = *value.running_revision;
  }
  result["sync"] = fixtureRuntimeSyncName(value.sync);
  if (value.target) result["target"] = fixtureTargetName(*value.target);
  if (value.draft) result["draft"] = *value.draft;
  if (value.message) result["message"] = *value.message;
  return result;
}

graphql::JVal fixtureProjectJson(
    const StudioProjectProjectionV1& value) {
  graphql::JVal result;
  result["projectId"] = value.project_id;
  result["name"] = value.name;
  if (value.description) result["description"] = *value.description;
  result["kind"] = fixtureProjectKindName(value.kind);
  result["revision"] = value.revision;
  graphql::JArray files;
  for (const auto& fileValue : value.files) {
    graphql::JVal item;
    item["target"] = fixtureTargetName(fileValue.target);
    item["path"] = fileValue.path;
    item["contentHash"] = fileValue.content_hash;
    item["byteLength"] =
        static_cast<std::int64_t>(fileValue.byte_length);
    files.emplace_back(std::move(item));
  }
  result["files"] = std::move(files);
  if (value.server_module_name) {
    result["serverModuleName"] = *value.server_module_name;
  }
  if (value.client_module_name) {
    result["clientModuleName"] = *value.client_module_name;
  }
  result["pairingPreference"] =
      fixturePairingName(value.pairing_preference);
  result["updatedAt"] = value.updated_at;
  return result;
}

graphql::JVal fixtureStudioOutputJson(
    std::string_view name, const NativeToolOutputV1& output) {
  if (name == "studio.context.get") {
    const auto& value = std::get<StudioContextV1>(output);
    graphql::JVal result;
    result["appRef"] = value.app_ref;
    if (value.project_ref) result["projectRef"] = *value.project_ref;
    result["gridRef"] = value.grid_ref;
    result["contextVersion"] = value.context_version;
    result["saveState"] = fixtureSaveStateName(value.save_state);
    result["runtime"] = fixtureRuntimeJson(value.runtime);
    if (value.client_epoch) result["clientEpoch"] = *value.client_epoch;
    graphql::JArray kinds;
    for (const auto kind : value.lease_kinds) {
      kinds.emplace_back(
          kind == LeaseKindV1::Workspace ? "WORKSPACE" : "PLAY");
    }
    result["leaseKinds"] = std::move(kinds);
    if (value.host_capability_revision) {
      result["hostCapabilityRevision"] =
          *value.host_capability_revision;
    }
    return result;
  }
  if (name == "studio.state.get") {
    const auto& value = std::get<StudioStateV1>(output);
    graphql::JVal result;
    if (value.project) {
      result["project"] = fixtureProjectJson(*value.project);
    }
    graphql::JArray files;
    for (const auto& fileValue : value.open_files) {
      graphql::JVal item;
      item["source"] = fixtureSourceName(fileValue.source);
      item["target"] = fixtureTargetName(fileValue.target);
      item["path"] = fileValue.path;
      files.emplace_back(std::move(item));
    }
    result["openFiles"] = std::move(files);
    result["saveState"] = fixtureSaveStateName(value.save_state);
    result["runtime"] = fixtureRuntimeJson(value.runtime);
    return result;
  }
  if (name == "project.select") {
    const auto& value =
        std::get<StudioProjectSelectResultV1>(output);
    return graphql::JVal::object(
        {{"selectedProjectRef", value.selected_project_ref},
         {"revision", value.revision}});
  }
  if (name == "workspace.tab.open" ||
      name == "workspace.tab.close") {
    return graphql::JVal::object(
        {{"ok", std::get<StudioOkV1>(output).ok}});
  }
  if (name == "diagnostics.local.get") {
    const auto& value = std::get<StudioDiagnosticsV1>(output);
    graphql::JVal result;
    graphql::JArray diagnostics;
    for (const auto& diagnostic : value.diagnostics) {
      graphql::JVal item;
      item["source"] =
          diagnostic.source == StudioDiagnosticSourceV1::LocalAdvisory
              ? "LOCAL_ADVISORY"
              : diagnostic.source == StudioDiagnosticSourceV1::Rustc
                    ? "RUSTC"
                    : "RUNTIME";
      item["target"] = fixtureTargetName(diagnostic.target);
      item["path"] = diagnostic.path;
      item["line"] = static_cast<std::int64_t>(diagnostic.line);
      item["column"] = static_cast<std::int64_t>(diagnostic.column);
      switch (diagnostic.severity) {
        case StudioDiagnosticSeverityV1::Error:
          item["severity"] = "ERROR";
          break;
        case StudioDiagnosticSeverityV1::Warning:
          item["severity"] = "WARNING";
          break;
        case StudioDiagnosticSeverityV1::Info:
          item["severity"] = "INFO";
          break;
        case StudioDiagnosticSeverityV1::Hint:
          item["severity"] = "HINT";
          break;
      }
      if (diagnostic.code) item["code"] = *diagnostic.code;
      item["message"] = diagnostic.message;
      diagnostics.emplace_back(std::move(item));
    }
    result["diagnostics"] = std::move(diagnostics);
    return result;
  }
  if (name == "runtime.status.get") {
    return fixtureRuntimeJson(
        std::get<StudioRuntimeStatusV1>(output));
  }
  if (name == "runtime.test_draft" ||
      name == "runtime.deploy_live") {
    const auto& value =
        std::get<StudioRuntimePlanResultV1>(output);
    graphql::JVal result;
    result["runtime"] = fixtureRuntimeJson(value.runtime);
    graphql::JArray targets;
    for (const auto targetValue : value.targets) {
      targets.emplace_back(fixtureTargetName(targetValue));
    }
    result[name == "runtime.test_draft" ? "compiledTargets"
                                         : "deployedTargets"] =
        std::move(targets);
    return result;
  }
  if (name == "runtime.invoke") {
    const auto& value =
        std::get<StudioRuntimeInvokeResultV1>(output);
    graphql::JVal result;
    result["resultType"] =
        value.result_type == StudioRuntimeResultTypeV1::Empty
            ? "EMPTY"
            : value.result_type == StudioRuntimeResultTypeV1::Text
                  ? "TEXT"
                  : "BASE64";
    result["result"] = value.result;
    result["fuelUsed"] = value.fuel_used;
    result["durationUs"] =
        static_cast<std::int64_t>(value.duration_us);
    return result;
  }
  const auto& value =
      std::get<StudioRuntimeStopResultV1>(output);
  graphql::JVal result;
  result["serverStopped"] = value.server_stopped;
  result["clientStopped"] = value.client_stopped;
  graphql::JArray failures;
  for (const auto& failure : value.failures) {
    failures.emplace_back(failure);
  }
  result["failures"] = std::move(failures);
  return result;
}

StudioTargetV1 fixtureTarget(std::string_view value) {
  CHECK(value == "SERVER" || value == "CLIENT");
  return value == "SERVER" ? StudioTargetV1::Server
                           : StudioTargetV1::Client;
}

std::vector<StudioTargetV1> fixtureTargets(
    const graphql::Json& values) {
  std::vector<StudioTargetV1> result;
  values.forEach([&](const graphql::Json& value) {
    result.push_back(fixtureTarget(value.asStringView()));
  });
  return result;
}

NativeToolArgumentsV1 fixtureArguments(
    std::string_view name, const graphql::Json& input) {
  if (name == "studio.context.get" ||
      name == "studio.state.get" ||
      name == "diagnostics.local.get" ||
      name == "runtime.status.get" || name == "runtime.stop") {
    return NoArgumentsV1{};
  }
  if (name == "project.select") {
    return StudioProjectSelectRequestV1{
        .project_ref = input["projectRef"].asString()};
  }
  if (name == "workspace.tab.open" ||
      name == "workspace.tab.close") {
    StudioFileTabRequestV1 request;
    const auto source = input["source"].asStringView();
    request.source =
        source == "PROJECT"
            ? StudioFileSourceV1::Project
            : source == "PERSONAL_LIBRARY"
                  ? StudioFileSourceV1::PersonalLibrary
                  : StudioFileSourceV1::Common;
    request.target = fixtureTarget(input["target"].asStringView());
    request.path = input["path"].asString();
    if (input["referenceRef"].ok()) {
      request.reference_ref = input["referenceRef"].asString();
    }
    return request;
  }
  if (name == "runtime.test_draft") {
    return StudioRuntimeTestDraftRequestV1{
        .expected_revision =
            input["expectedRevision"].asString(),
        .targets = fixtureTargets(input["targets"])};
  }
  if (name == "runtime.deploy_live") {
    const auto pairing = input["pairingPreference"].asStringView();
    return StudioRuntimeDeployLiveRequestV1{
        .expected_revision =
            input["expectedRevision"].asString(),
        .project_content_hash =
            input["projectContentHash"].asString(),
        .targets = fixtureTargets(input["targets"]),
        .pairing_preference =
            pairing == "NONE"
                ? StudioPairingPreferenceV1::None
                : pairing == "OPTIONAL"
                      ? StudioPairingPreferenceV1::Optional
                      : StudioPairingPreferenceV1::Required,
        .draft = input["draft"].asBool()};
  }
  CHECK(name == "runtime.invoke");
  StudioRuntimeInvokeRequestV1 request;
  request.export_name = input["exportName"].asString();
  request.environment =
      input["environment"].asStringView() == "DRAFT"
          ? StudioRuntimeEnvironmentV1::Draft
          : StudioRuntimeEnvironmentV1::Live;
  input["params"].forEach([&](const graphql::Json& parameter) {
    const auto type = parameter["type"].asStringView();
    request.params.push_back({
        .name = parameter["name"].asString(),
        .type = type == "STRING"
                    ? StudioRuntimeParameterTypeV1::String
                    : type == "DECIMAL"
                          ? StudioRuntimeParameterTypeV1::Decimal
                          : StudioRuntimeParameterTypeV1::Boolean,
        .value = parameter["value"].asString(),
    });
  });
  return request;
}

const NativeLocalToolContractV1& fixtureContract(
    std::string_view name) {
  for (const auto& contract : nativeLocalToolContractsV1()) {
    if (contract.name == name) return contract;
  }
  CHECK(false);
  return nativeLocalToolContractsV1().front();
}

NativeToolInvocationV1 fixtureInvocation(
    const graphql::Json& operation, std::size_t sequence) {
  const std::string name = operation["tool"].asString();
  const auto& contract = fixtureContract(name);
  NativeToolInvocationV1 invocation;
  invocation.session_id = "session-1";
  invocation.run_id = "run-1";
  invocation.tool_call_id =
      "fixture-" + std::to_string(sequence);
  invocation.name = name;
  invocation.descriptor_digest =
      std::string(contract.descriptor_digest);
  invocation.arguments =
      fixtureArguments(name, operation["input"]);
  invocation.argument_hash =
      "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  invocation.context_version = "context-1";
  invocation.client_epoch = "1";
  if (operation["leaseId"].ok()) {
    invocation.lease_id = operation["leaseId"].asString();
  }
  if (operation["approvalGrant"].ok()) {
    invocation.approval_grant =
        operation["approvalGrant"].asString();
  }
  invocation.deadline =
      operation["deadlineMs"].asInt64() == 1
          ? "2026-07-24T12:00:00.001Z"
          : "2026-07-24T12:02:00.000Z";
  return invocation;
}

NativeToolDispatcherOptionsV1 fixtureDispatcherOptions(
    const FakeClock& clock) {
  NativeToolDispatcherOptionsV1 options;
  options.clock = &clock;
  options.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.context_version = [] { return std::string("context-1"); };
  options.mode = [] { return NativeAgentModeV1::Build; };
  options.is_lease_active =
      [](std::string_view id, LeaseKindV1 kind) {
        return (kind == LeaseKindV1::Workspace &&
                id == "workspace-lease") ||
               (kind == LeaseKindV1::Play &&
                id == "play-lease");
      };
  options.validate_approval_grant =
      [](const NativeToolInvocationV1& invocation) {
        return invocation.approval_grant == "approved";
      };
  return options;
}

std::vector<CrowdyStudioEditorDiagnostic>
fixtureLocalDiagnostics(bool enabled) {
  if (!enabled) return {};
  return {{
      .source = CrowdyStudioEditorDiagnosticSource::LocalAdvisory,
      .target = CrowdyStudioTarget::Server,
      .path = "src/lib.rs",
      .line = 3,
      .column = 5,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Warning,
      .code = "unused",
      .message = "unused value",
  }};
}

CrowdyStudioControllerHostAdapterOptions fixtureHostOptions(
    std::vector<CrowdyStudioEditorDiagnostic>* diagnostics) {
  auto options = hostOptions();
  options.localDiagnostics = [diagnostics] { return *diagnostics; };
  return options;
}

struct FixtureStudioHarness {
  FakeClock clock;
  FixtureCrypto crypto;
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  std::vector<CrowdyStudioEditorDiagnostic> diagnostics;
  FakePlayerHost playerHost;
  AgentControlLeaseManager leases;
  CrowdyStudioController controller;
  CrowdyStudioControllerHostAdapter host;
  NativeToolDispatcherV1 dispatcher;
  std::size_t sequence = 0;

  explicit FixtureStudioHarness(bool withDiagnostics)
      : diagnostics(fixtureLocalDiagnostics(withDiagnostics)),
        leases(playerHost),
        controller(controllerOptions(clock), provider, runtime, crypto,
                   clock, nullptr, &approval),
        host(controller, crypto, fixtureHostOptions(&diagnostics)),
        dispatcher(leases, &host,
                   fixtureDispatcherOptions(clock)) {
    controller.initialize();
  }

  NativeToolResultV1 run(const graphql::Json& operation) {
    std::optional<NativeToolResultV1> result;
    dispatcher.dispatch(
        fixtureInvocation(operation, ++sequence),
        [&](NativeToolResultV1 value) {
          result = std::move(value);
        });
    CHECK(result.has_value());
    return std::move(*result);
  }
};

std::string_view fixtureStatusName(NativeToolResultStatusV1 status) {
  switch (status) {
    case NativeToolResultStatusV1::Succeeded: return "SUCCEEDED";
    case NativeToolResultStatusV1::Failed: return "FAILED";
    case NativeToolResultStatusV1::Cancelled: return "CANCELLED";
    case NativeToolResultStatusV1::TimedOut: return "TIMED_OUT";
    case NativeToolResultStatusV1::OutcomeUnknown:
      return "OUTCOME_UNKNOWN";
  }
  return "";
}

void checkFixtureResult(const NativeToolResultV1& actual,
                        const graphql::Json& fixtureCase) {
  const auto expected = fixtureCase["expected"];
  CHECK(fixtureStatusName(actual.status) ==
        expected["status"].asStringView());
  if (expected["errorCode"].ok()) {
    CHECK(actual.error.has_value());
    CHECK(actual.error->code ==
          expected["errorCode"].asString());
  } else {
    CHECK(!actual.error);
  }
  if (expected["output"].ok()) {
    CHECK(actual.output.has_value());
    const auto projected = graphql::Json::parse(
        fixtureStudioOutputJson(
            fixtureCase["tool"].asStringView(), *actual.output)
            .dump());
    CHECK(projected.ok());
    const std::string actualJson = agent::canonicalJson(projected);
    const std::string expectedJson =
        agent::canonicalJson(expected["output"]);
    if (actualJson != expectedJson) {
      std::fprintf(
          stderr,
          "Studio fixture mismatch for %s\nactual: %s\nexpected: %s\n",
          fixtureCase["name"].asString().c_str(), actualJson.c_str(),
          expectedJson.c_str());
    }
    CHECK(actualJson == expectedJson);
  } else {
    CHECK(!actual.output);
  }
}

class HoldingStudioHost final : public CrowdyStudioHostAdapter {
 public:
  void dispatch(StudioNativeToolKindV1,
                const StudioNativeToolRequestV1&,
                const ValidatedStudioGateV1&,
                CancellationTokenV1,
                StudioToolCallbackV1 callback) override {
    pending = std::move(callback);
  }

  void clearAgentOperation(PreemptionReasonV1) noexcept override {
    ++clears;
  }

  std::optional<StudioToolCallbackV1> pending;
  int clears = 0;
};

void testSharedStudioHostFixture() {
  const graphql::Json fixture = graphql::Json::parse(
      studioFixtureText("crowdyjs-studio-host-tools.v1.json"));
  CHECK(fixture.ok());
  CHECK(fixture["fixtureVersion"].asInt64() == 1);
  CHECK(fixture["contractVersion"].asStringView() ==
        "crowdy.studio-host-tools/1");
  CHECK(fixture["crowdyJs"]["version"].asStringView() == "12.1.0");
  CHECK(fixture["crowdyJs"]["commit"].asStringView() ==
        "a510fcecf43bf9365fc34631a64fd201382214e7");
  CHECK_EQ(fixture["toolNames"].size(), std::size_t{11});

  std::size_t successCount = 0;
  fixture["cases"].forEach([&](const graphql::Json& fixtureCase) {
    const auto category = fixtureCase["category"].asStringView();
    if (category != "success" && category != "approval") return;
    FixtureStudioHarness harness(
        fixtureCase["tool"].asStringView() ==
        "diagnostics.local.get");
    if (fixtureCase["setup"].ok()) {
      fixtureCase["setup"].forEach(
          [&](const graphql::Json& setup) {
            const auto result = harness.run(setup);
            CHECK(result.status ==
                  NativeToolResultStatusV1::Succeeded);
          });
    }
    const auto result = harness.run(fixtureCase);
    checkFixtureResult(result, fixtureCase);
    if (category == "success") ++successCount;
  });
  CHECK_EQ(successCount, std::size_t{11});

  fixture["cases"].forEach([&](const graphql::Json& fixtureCase) {
    const auto category = fixtureCase["category"].asStringView();
    if (category != "cancellation" &&
        category != "outcome-unknown") {
      return;
    }
    FakeClock clock;
    FakePlayerHost playerHost;
    AgentControlLeaseManager leases(playerHost);
    HoldingStudioHost host;
    NativeToolDispatcherV1 dispatcher(
        leases, &host, fixtureDispatcherOptions(clock));
    std::optional<NativeToolResultV1> result;
    dispatcher.dispatch(
        fixtureInvocation(fixtureCase, 1),
        [&](NativeToolResultV1 value) {
          result = std::move(value);
        });
    CHECK(!result);
    if (category == "cancellation") {
      dispatcher.cancelActive(PreemptionReasonV1::HUMAN_INPUT);
    } else {
      clock.epoch += 2;
      clock.monotonic += 2;
      dispatcher.tick();
    }
    CHECK(result.has_value());
    checkFixtureResult(*result, fixtureCase);
    CHECK_EQ(host.clears, 1);
  });
}

void testEveryStudioToolMapping() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  auto options = controllerOptions(clock);
  CrowdyStudioController controller(
      std::move(options), provider, runtime, *crypto, clock, nullptr,
      &approval);
  controller.initialize();

  std::vector<CrowdyStudioEditorDiagnostic> diagnostics{{
      .source = CrowdyStudioEditorDiagnosticSource::Rustc,
      .target = CrowdyStudioTarget::Server,
      .path = "src/lib.rs",
      .line = 3,
      .column = 5,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Warning,
      .code = "unused",
      .message = "unused value",
  }};
  auto optionsForHost = hostOptions();
  optionsForHost.localDiagnostics = [&] { return diagnostics; };
  CrowdyStudioControllerHostAdapter host(
      controller, *crypto, std::move(optionsForHost));

  std::size_t mapped = 0;
  auto context = dispatch(
      host, StudioNativeToolKindV1::ContextGet, NoArgumentsV1{});
  CHECK(context.ok());
  CHECK(std::get<StudioContextV1>(*context.value).app_ref == "42");
  ++mapped;

  auto state =
      dispatch(host, StudioNativeToolKindV1::StateGet, NoArgumentsV1{});
  CHECK(state.ok());
  CHECK(std::get<StudioStateV1>(*state.value).project.has_value());
  CHECK_EQ(std::get<StudioStateV1>(*state.value).project->files.size(), 2U);
  ++mapped;

  auto selected = dispatch(
      host, StudioNativeToolKindV1::ProjectSelect,
      StudioProjectSelectRequestV1{.project_ref = "project-2"});
  CHECK(selected.ok());
  CHECK(std::get<StudioProjectSelectResultV1>(*selected.value)
            .selected_project_ref == "project-2");
  ++mapped;

  StudioFileTabRequestV1 tab{
      .source = StudioFileSourceV1::Project,
      .target = StudioTargetV1::Server,
      .path = "Cargo.toml",
      .reference_ref = std::nullopt,
  };
  auto opened = dispatch(
      host, StudioNativeToolKindV1::WorkspaceTabOpen, tab);
  CHECK(opened.ok());
  CHECK(std::get<StudioOkV1>(*opened.value).ok);
  ++mapped;
  auto closed = dispatch(
      host, StudioNativeToolKindV1::WorkspaceTabClose, tab);
  CHECK(closed.ok());
  ++mapped;

  auto local = dispatch(
      host, StudioNativeToolKindV1::DiagnosticsLocalGet,
      NoArgumentsV1{});
  CHECK(local.ok());
  CHECK_EQ(std::get<StudioDiagnosticsV1>(*local.value).diagnostics.size(),
           1U);
  ++mapped;

  auto status = dispatch(
      host, StudioNativeToolKindV1::RuntimeStatusGet, NoArgumentsV1{});
  CHECK(status.ok());
  CHECK(std::get<StudioRuntimeStatusV1>(*status.value).saved_revision ==
        "2");
  ++mapped;

  auto draft = dispatch(
      host, StudioNativeToolKindV1::RuntimeTestDraft,
      StudioRuntimeTestDraftRequestV1{
          .expected_revision = "2",
          .targets = {StudioTargetV1::Server}});
  CHECK(draft.ok());
  CHECK(std::get<StudioRuntimePlanResultV1>(*draft.value)
            .runtime.draft == std::optional<bool>(true));
  ++mapped;

  auto invoked = dispatch(
      host, StudioNativeToolKindV1::RuntimeInvoke,
      StudioRuntimeInvokeRequestV1{
          .export_name = "invoke",
          .environment = StudioRuntimeEnvironmentV1::Draft,
          .params = {{.name = "enabled",
                      .type = StudioRuntimeParameterTypeV1::Boolean,
                      .value = "true"}}});
  CHECK(invoked.ok());
  CHECK(std::get<StudioRuntimeInvokeResultV1>(*invoked.value).result ==
        R"({"ok":true})");
  CHECK(runtime.calls.back() == R"(invoke:{"enabled":true})");
  ++mapped;

  auto stopped = dispatch(
      host, StudioNativeToolKindV1::RuntimeStop, NoArgumentsV1{});
  CHECK(stopped.ok());
  CHECK(std::get<StudioRuntimeStopResultV1>(*stopped.value)
            .server_stopped);
  ++mapped;

  const auto exact = controller.makeDeploymentPlan();
  auto liveGate = gate("context-1", "approved");
  auto deployed = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive,
      StudioRuntimeDeployLiveRequestV1{
          .expected_revision = exact.expectedRevisionId,
          .project_content_hash = *exact.projectContentHash,
          .targets = {StudioTargetV1::Server},
          .pairing_preference = StudioPairingPreferenceV1::None,
          .draft = false},
      liveGate);
  CHECK(deployed.ok());
  CHECK_EQ(approval.live, 1);
  ++mapped;

  auto playGate = liveGate;
  playGate.lease_id = "play-lease";
  auto liveInvoke = dispatch(
      host, StudioNativeToolKindV1::RuntimeInvoke,
      StudioRuntimeInvokeRequestV1{
          .export_name = "invoke",
          .environment = StudioRuntimeEnvironmentV1::Live,
          .params = {}},
      playGate);
  CHECK(liveInvoke.ok());

  CHECK_EQ(mapped, 11U);
}

void testGateCancellationHashAndPreemptionFencing() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  auto options = controllerOptions(clock);
  CrowdyStudioController controller(
      std::move(options), provider, runtime, *crypto, clock, nullptr,
      &approval);
  controller.initialize();
  CrowdyStudioControllerHostAdapter host(
      controller, *crypto, hostOptions());

  CrowdyStudioControllerHostAdapter unboundHost(
      controller, *crypto);
  auto unbound = dispatch(
      unboundHost, StudioNativeToolKindV1::StateGet, NoArgumentsV1{});
  CHECK(!unbound.ok());
  CHECK(unbound.error->code == "AGENT_HOST_UNAVAILABLE");

  auto stale = dispatch(
      host, StudioNativeToolKindV1::StateGet, NoArgumentsV1{},
      gate("stale-context"));
  CHECK(!stale.ok());
  CHECK(stale.error->code == "AGENT_CONTEXT_STALE");

  const auto exact = controller.makeDeploymentPlan();
  const auto calls = runtime.calls.size();
  auto wrongHash = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive,
      StudioRuntimeDeployLiveRequestV1{
          .expected_revision = exact.expectedRevisionId,
          .project_content_hash =
              "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          .targets = {StudioTargetV1::Server},
          .pairing_preference = StudioPairingPreferenceV1::None,
          .draft = false},
      gate("context-1", "approved"));
  CHECK(!wrongHash.ok());
  CHECK(wrongHash.error->code == "AGENT_CONTEXT_STALE");
  CHECK_EQ(runtime.calls.size(), calls);

  auto approvalMismatch = gate("context-1", "wrong");
  auto denied = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive,
      StudioRuntimeDeployLiveRequestV1{
          .expected_revision = exact.expectedRevisionId,
          .project_content_hash = *exact.projectContentHash,
          .targets = {StudioTargetV1::Server},
          .pairing_preference = StudioPairingPreferenceV1::None,
          .draft = false},
      approvalMismatch);
  CHECK(!denied.ok());
  CHECK(denied.error->code == "AGENT_APPROVAL_MISMATCH");
  CHECK_EQ(runtime.calls.size(), calls);

  CancellationSourceV1 cancellation;
  cancellation.cancel();
  std::optional<AdapterResultV1<StudioNativeToolOutputV1>> cancelled;
  host.dispatch(
      StudioNativeToolKindV1::StateGet, NoArgumentsV1{}, gate(),
      cancellation.token(),
      [&](auto value) { cancelled = std::move(value); });
  CHECK(cancelled && !cancelled->ok());
  CHECK(cancelled->error->code == "AGENT_CANCELLED");

  runtime.onVersions = [&] {
    host.clearAgentOperation(PreemptionReasonV1::HUMAN_INPUT);
  };
  auto preempted = dispatch(
      host, StudioNativeToolKindV1::RuntimeTestDraft,
      StudioRuntimeTestDraftRequestV1{
          .expected_revision = "1",
          .targets = {StudioTargetV1::Server}});
  CHECK(!preempted.ok());
  CHECK(preempted.outcome_unknown);
  CHECK(preempted.error->code == "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(controller.getState().agentActivity ==
        CrowdyStudioAgentActivity::Paused);
}

void testEditorRoundTripsPollTickAndRelayout() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto editor = std::make_shared<FakeEditor>();
  FakePlayerHost playerHost;
  AgentControlLeaseManager leases(playerHost);

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.editor = editor;
  options.leaseManager = &leases;
  options.nativeTools.clock = &clock;
  options.nativeTools.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.nativeTools.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.nativeTools.context_version =
      [] { return std::string("context-1"); };
  options.nativeTools.mode =
      [] { return NativeAgentModeV1::Build; };
  options.nativeTools.is_lease_active =
      [](std::string_view, LeaseKindV1) { return true; };
  options.studioHost = hostOptions(editor.get());
  int polls = 0;
  options.platformPoll = [&] {
    ++polls;
    return std::size_t{2};
  };

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  integration->initialize();
  CHECK(!editor->snapshots.empty());
  const auto& initial = editor->snapshots.back();
  CHECK(initial.projectId == std::optional<std::string>("project-1"));
  CHECK_EQ(initial.buffers.size(), 4U);
  CHECK_EQ(initial.openFiles.size(), 1U);
  CHECK(initial.selectedFile.has_value());

  editor->callbacks.onProjectFileChange(
      CrowdyStudioTarget::Server, "src/lib.rs",
      "pub fn edited() {}");
  CHECK(integration->studio().fileContent({
            CrowdyStudioFileRef::Source::Project,
            CrowdyStudioTarget::Server,
            "src/lib.rs",
            std::nullopt}) == "pub fn edited() {}");

  editor->callbacks.onOpenFile({
      CrowdyStudioFileRef::Source::Project,
      CrowdyStudioTarget::Server,
      "Cargo.toml",
      std::nullopt});
  CHECK_EQ(integration->studio().getState().openFiles.size(), 2U);
  editor->callbacks.onCloseFile({
      CrowdyStudioFileRef::Source::Project,
      CrowdyStudioTarget::Server,
      "Cargo.toml",
      std::nullopt});
  CHECK_EQ(integration->studio().getState().openFiles.size(), 1U);

  editor->callbacks.onLocalDiagnostics({{
      .source = CrowdyStudioEditorDiagnosticSource::LocalAdvisory,
      .target = CrowdyStudioTarget::Server,
      .path = "src/lib.rs",
      .line = 1,
      .column = 1,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Hint,
      .code = std::nullopt,
      .message = "consider a comment",
  }});
  CHECK_EQ(integration->editor()->localDiagnostics().size(), 1U);
  CHECK_EQ(integration->studio().getState().localDiagnostics.size(), 1U);

  integration->relayout();
  CHECK_EQ(editor->relayouts, 1);
  CHECK_EQ(integration->poll(), std::size_t{2});
  CHECK_EQ(polls, 1);
  clock.monotonic = 6;
  CHECK_EQ(integration->tick(), std::size_t{2});
  CHECK_EQ(provider->saves, 1);
  CHECK_EQ(polls, 2);

  integration->dispose();
  CHECK(editor->disposed);
  CHECK(!editor->callbacks.onProjectFileChange);
}

void testIntegrationOwnershipAndExtensionSlots() {
  auto events = std::make_shared<std::vector<std::string>>();
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>(events);
  auto runtime = std::make_shared<FakeRuntime>(events);
  auto editor = std::make_shared<FakeEditor>(events);
  auto clientRuntime = std::make_shared<FakeClientRuntime>(events);
  auto layout = std::make_shared<FakeLayout>(events);
  auto control = std::make_shared<FakeControl>(events);
  std::weak_ptr<FakeProjectProvider> providerWeak = provider;
  std::weak_ptr<FakeRuntime> runtimeWeak = runtime;
  std::weak_ptr<FakeEditor> editorWeak = editor;
  std::weak_ptr<FakeClientRuntime> clientRuntimeWeak = clientRuntime;
  FakePlayerHost playerHost;
  AgentControlLeaseManager leases(playerHost);

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.editor = editor;
  options.clientRuntime = clientRuntime;
  options.leaseManager = &leases;
  options.layout = layout;
  options.control = control;
  options.nativeTools.clock = &clock;
  options.nativeTools.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.nativeTools.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.nativeTools.context_version =
      [] { return std::string("context-1"); };
  options.studioHost = hostOptions(editor.get());

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  provider.reset();
  runtime.reset();
  editor.reset();
  clientRuntime.reset();
  layout.reset();
  control.reset();
  crypto.reset();
  CHECK(!providerWeak.expired());
  CHECK(!runtimeWeak.expired());
  CHECK(!editorWeak.expired());
  CHECK(!clientRuntimeWeak.expired());

  integration->initialize();
  integration->relayout();
  integration->tick();
  events->clear();
  integration.reset();

  CHECK(providerWeak.expired());
  CHECK(runtimeWeak.expired());
  CHECK(editorWeak.expired());
  CHECK(clientRuntimeWeak.expired());
  const auto controlDispose =
      std::find(events->begin(), events->end(), "control-dispose");
  const auto layoutDispose =
      std::find(events->begin(), events->end(), "layout-dispose");
  const auto editorDispose =
      std::find(events->begin(), events->end(), "editor-dispose");
  const auto runtimeStop =
      std::find(events->begin(), events->end(), "runtime-stop");
  CHECK(controlDispose != events->end());
  CHECK(layoutDispose != events->end());
  CHECK(editorDispose != events->end());
  CHECK(runtimeStop != events->end());
  CHECK(controlDispose < editorDispose);
  CHECK(layoutDispose < editorDispose);
  CHECK(editorDispose < runtimeStop);
}

void testCrowdyClientConstructionHelper() {
  auto crypto = std::make_shared<FakeCrypto>();
  auto transport = std::make_shared<FakeHttpTransport>();
  FakeClock clock;
  FakePlayerHost playerHost;
  AgentControlLeaseManager leases(playerHost);
  std::unique_ptr<CrowdyStudioIntegration> integration;
  {
    ClientConfig config;
    config.httpUrl = "https://game.invalid";
    config.managementUrl = "https://management.invalid";
    config.transport = transport;
    config.crypto = crypto.get();
    CrowdyClient client(std::move(config));

    CrowdyStudioIntegrationOptions options;
    options.studio = controllerOptions(clock);
    options.crypto = crypto;
    options.clock = &clock;
    options.leaseManager = &leases;
    CrowdyStudioAgentControllerOptions agentOptions;
    agentOptions.sessionId = "session-1";
    options.agent = std::move(agentOptions);
    integration =
        client.createCrowdyStudioIntegration(std::move(options));
    CHECK(integration);
    CHECK(integration->agentController() != nullptr);
  }
  CHECK(integration->agentController() != nullptr);
  (void)integration->tick();
  integration->dispose();
}

}  // namespace

int main() {
  testSharedStudioHostFixture();
  testEveryStudioToolMapping();
  testGateCancellationHashAndPreemptionFencing();
  testEditorRoundTripsPollTickAndRelayout();
  testIntegrationOwnershipAndExtensionSlots();
  testCrowdyClientConstructionHelper();
  std::printf("studio_integration_test passed\n");
  return 0;
}
