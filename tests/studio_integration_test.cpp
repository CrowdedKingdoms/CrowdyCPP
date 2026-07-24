#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
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

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = 1'753'393'600'000LL;
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
  value.metadata.name = "Native Studio";
  value.metadata.serverModuleName = "native-studio";
  value.files = {
      file(CrowdyStudioTarget::Server, "Cargo.toml", "[package]"),
      file(CrowdyStudioTarget::Server, "src/lib.rs", "pub fn invoke() {}"),
  };
  value.sdkVersion = "0.1.5";
  value.revision = {std::move(revision), "2026-07-24T00:00:00.000Z"};
  value.fileCount = 2;
  value.totalBytes = "30";
  value.createdAt = "2026-07-24T00:00:00.000Z";
  value.updatedAt = "2026-07-24T00:00:00.000Z";
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
  testEveryStudioToolMapping();
  testGateCancellationHashAndPreemptionFencing();
  testEditorRoundTripsPollTickAndRelayout();
  testIntegrationOwnershipAndExtensionSlots();
  testCrowdyClientConstructionHelper();
  std::printf("studio_integration_test passed\n");
  return 0;
}
