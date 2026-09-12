#include <algorithm>
#include <cstdio>
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
static_assert(!HasGraphqlClient<CrowdyStudioIntegration>);
static_assert(!HasFilesystem<ICrowdyStudioEditorAdapter>);
static_assert(!HasFilesystem<CrowdyStudioIntegration>);
static_assert(!HasShell<ICrowdyStudioEditorAdapter>);
static_assert(!HasShell<CrowdyStudioIntegration>);

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
  bool failEnable = false;
  bool failInvoke = false;

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
    if (failEnable && enabled) {
      throw std::runtime_error(
          "server enable response was lost");
    }
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
    if (failInvoke) {
      throw std::runtime_error(
          "runtime invoke response was lost");
    }
    return {std::nullopt, R"({"ok":true})", "4", 2};
  }
};

class FakeApproval final : public ICrowdyStudioApprovalGate {
 public:
  int live = 0;
  int restores = 0;
  bool failLive = false;
  bool failRestore = false;
  void requireLiveApproval(
      const CrowdyStudioLiveApprovalRequest& request,
      std::string_view grant) override {
    CHECK(request.projectContentHash.rfind("sha256:", 0) == 0);
    CHECK(grant == "approved");
    if (failLive) {
      throw std::runtime_error(
          "approval provider validation failed");
    }
    ++live;
  }
  void requireRestoreApproval(
      const CrowdyStudioRestoreApprovalRequest& request,
      std::string_view grant) override {
    CHECK(!request.checkpointId.empty());
    CHECK(grant == "approved");
    if (failRestore) {
      throw std::runtime_error(
          "restore approval provider validation failed");
    }
    ++restores;
  }
};

class FakeSynchronization final
    : public ICrowdyStudioSynchronizationProvider {
 public:
  CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioProjectScope&, std::string_view,
      const CrowdyStudioAtomicPatchInput&) override {
    return {};
  }
  std::vector<CrowdyStudioCheckpointMetadata> listCheckpoints(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return {};
  }
  CrowdyStudioCheckpointRestoreResult restoreCheckpoint(
      const CrowdyStudioCheckpointRestoreInput&) override {
    ++restores;
    throw std::runtime_error(
        "restore response was lost");
  }

  int restores = 0;
};

class SuccessfulSynchronization final
    : public ICrowdyStudioSynchronizationProvider {
 public:
  explicit SuccessfulSynchronization(FakeProjectProvider& provider)
      : provider_(provider) {}

  CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioProjectScope&, std::string_view,
      const CrowdyStudioAtomicPatchInput&) override {
    throw std::runtime_error("atomic patch is outside this restore test");
  }

  std::vector<CrowdyStudioCheckpointMetadata> listCheckpoints(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return {{
        .checkpointId = "checkpoint-1",
        .projectRevisionId = provider_.projects.front().revision.id,
        .contentHash =
            "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .reason = CrowdyStudioCheckpointMetadata::Reason::Manual,
        .files = {},
        .createdAt = "2026-07-24T00:00:00.000Z",
        .restoredAt = std::nullopt,
    }};
  }

  CrowdyStudioCheckpointRestoreResult restoreCheckpoint(
      const CrowdyStudioCheckpointRestoreInput& input) override {
    CHECK(input.checkpointId == "checkpoint-1");
    CHECK(input.approvalGrant == "approved");
    auto restored = provider_.projects.front();
    CHECK(restored.revision.id == input.expectedRevisionId);
    CrowdyStudioCheckpointMetadata preimage;
    preimage.checkpointId = "pre-restore-1";
    preimage.projectRevisionId = restored.revision.id;
    preimage.contentHash =
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    preimage.reason =
        CrowdyStudioCheckpointMetadata::Reason::RestorePreimage;
    preimage.createdAt = "2026-07-24T00:00:01.000Z";
    restored.revision.id =
        std::to_string(std::stoll(restored.revision.id) + 1);
    restored.updatedAt = "2026-07-24T00:00:02.000Z";
    provider_.projects.front() = restored;
    ++restores;
    return {std::move(restored), std::move(preimage)};
  }

  int restores = 0;

 private:
  FakeProjectProvider& provider_;
};

class FakeWallet final : public ICrowdyStudioWalletProvider {
 public:
  CrowdyStudioWalletSnapshot balance() override {
    ++reads;
    if (fail) throw std::runtime_error("wallet unavailable");
    return {"1234", "USD"};
  }

  int reads = 0;
  bool fail = false;
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
  explicit FakePlayerHost(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {}

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
    if (events) events->push_back("host-clear");
  }
  std::shared_ptr<std::vector<std::string>> events;
  int clears = 0;
};

class FakeHttpTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse send(
      const graphql::HttpRequest&) override {
    return {503, R"({"errors":[{"message":"offline"}]})"};
  }
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

void testNonblockingPollAndScheduledMaintenance() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  FakePlayerHost playerHost;
  int platformPolls = 0;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.playerHost = &playerHost;
  options.platformPoll = [&] {
    ++platformPolls;
    return std::size_t{1};
  };

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  integration->initialize();
  CHECK(integration->playerHost() == &playerHost);

  // poll() is the nonblocking lane: it pumps the platform and nothing else.
  int ran = 0;
  integration->schedule([&] { ++ran; });
  CHECK_EQ(integration->pendingStudioMaintenance(), 1U);
  CHECK_EQ(integration->poll(), std::size_t{1});
  CHECK_EQ(ran, 0);
  CHECK_EQ(platformPolls, 1);

  // runStudioMaintenance() is the blocking lane: scheduled work, then the
  // controller's own maintenance tick.
  CHECK_EQ(integration->runStudioMaintenance(), 1U);
  CHECK_EQ(ran, 1);
  CHECK_EQ(integration->pendingStudioMaintenance(), 0U);
  CHECK_EQ(integration->studio().getState().project->projectId,
           "project-1");

  // The player host is observation-only: nothing here dispatched to it.
  CHECK_EQ(playerHost.clears, 0);
}

void testEditorRoundTripsPollTickAndRelayout() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto editor = std::make_shared<FakeEditor>();
  FakePlayerHost playerHost;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.editor = editor;
  options.playerHost = &playerHost;
  int polls = 0;
  options.platformPoll = [&] {
    ++polls;
    return std::size_t{2};
  };

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  CHECK(integration->layoutSnapshot().isVisible(
      StudioPaneId::Explorer));
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
      .target = CrowdyStudioTarget::Client,
      .path = "./src/client.rs",
      .line = 7,
      .column = 11,
      .endLine = 8,
      .endColumn = 13,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Hint,
      .message = "consider a comment",
      .code = "native-hint",
      .source = CrowdyStudioEditorDiagnosticSource::Runtime,
  }});
  CHECK_EQ(integration->editor()->localDiagnostics().size(), 1U);
  CHECK_EQ(integration->studio().getState().localDiagnostics.size(), 1U);
  const auto& diagnostic =
      integration->studio().getState().localDiagnostics.front();
  CHECK_EQ(diagnostic.target, CrowdyStudioTarget::Client);
  CHECK_EQ(diagnostic.path, "src/client.rs");
  CHECK_EQ(diagnostic.line, 7U);
  CHECK_EQ(diagnostic.column, 11U);
  CHECK(diagnostic.endLine == std::optional<std::uint32_t>(8));
  CHECK(diagnostic.endColumn == std::optional<std::uint32_t>(13));
  CHECK_EQ(diagnostic.source, CrowdyStudioDiagnosticSource::Runtime);
  CHECK(diagnostic.code == std::optional<std::string>("native-hint"));

  integration->relayout();
  CHECK_EQ(editor->relayouts, 1);
  CHECK_EQ(integration->poll(), std::size_t{2});
  CHECK_EQ(polls, 1);
  clock.monotonic = 6;
  CHECK_EQ(integration->tick(), std::size_t{2});
  CHECK_EQ(provider->saves, 0);
  CHECK_EQ(polls, 2);
  CHECK_EQ(integration->runStudioMaintenance(), std::size_t{0});
  CHECK_EQ(provider->saves, 1);

  integration->dispose();
  CHECK(editor->disposed);
  CHECK(!editor->callbacks.onProjectFileChange);
}

void testOwnedWalletProviderIsNonfatal() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto wallet = std::make_shared<FakeWallet>();
  std::weak_ptr<FakeWallet> walletWeak = wallet;
  FakePlayerHost playerHost;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.walletProvider = wallet;
  options.playerHost = &playerHost;
  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  wallet.reset();
  CHECK(!walletWeak.expired());

  integration->initialize();
  integration->studio().setSurfaceVisible(
      CrowdyStudioPolledSurface::Usage, true);
  CHECK(integration->studio().getState().wallet ==
        std::optional<CrowdyStudioWalletSnapshot>(
            CrowdyStudioWalletSnapshot{"1234", "USD"}));
  CHECK_EQ(walletWeak.lock()->reads, 1);

  walletWeak.lock()->fail = true;
  integration->studio().refreshSurface(
      CrowdyStudioPolledSurface::Usage);
  CHECK(!integration->studio().getState().wallet);
  CHECK_EQ(walletWeak.lock()->reads, 2);

  integration.reset();
  CHECK(walletWeak.expired());
}

void testIntegrationApprovedRestoreRequiresInjectedCapabilities() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto approval = std::make_shared<FakeApproval>();
  auto synchronization =
      std::make_shared<SuccessfulSynchronization>(*provider);
  FakePlayerHost playerHost;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.synchronization = synchronization;
  options.approval = approval;
  options.playerHost = &playerHost;

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  integration->initialize();
  CHECK_EQ(integration->studio().getState().checkpoints.size(), 1U);
  const std::string previous =
      integration->studio().getState().project->revision.id;
  const auto checkpoint = integration->studio().restoreCheckpoint(
      "checkpoint-1", "approved", previous);
  CHECK_EQ(checkpoint.checkpointId, "pre-restore-1");
  CHECK_EQ(checkpoint.projectRevisionId, previous);
  CHECK(integration->studio().getState().project->revision.id != previous);
  CHECK_EQ(approval->restores, 1);
  CHECK_EQ(synchronization->restores, 1);
}

void testConcreteIntegrationOwnershipAndDestructionOrder() {
  auto events = std::make_shared<std::vector<std::string>>();
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>(events);
  auto runtime = std::make_shared<FakeRuntime>(events);
  auto editor = std::make_shared<FakeEditor>(events);
  auto clientRuntime = std::make_shared<FakeClientRuntime>(events);
  auto layoutStorage =
      std::make_shared<InMemoryCrowdyStudioLayoutStorage>();
  std::weak_ptr<FakeProjectProvider> providerWeak = provider;
  std::weak_ptr<FakeRuntime> runtimeWeak = runtime;
  std::weak_ptr<FakeEditor> editorWeak = editor;
  std::weak_ptr<FakeClientRuntime> clientRuntimeWeak = clientRuntime;
  std::weak_ptr<InMemoryCrowdyStudioLayoutStorage>
      layoutStorageWeak = layoutStorage;
  FakePlayerHost playerHost(events);

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.editor = editor;
  options.clientRuntime = clientRuntime;
  options.layoutStorage = layoutStorage;
  options.playerHost = &playerHost;

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  provider.reset();
  runtime.reset();
  editor.reset();
  clientRuntime.reset();
  layoutStorage.reset();
  crypto.reset();
  CHECK(!providerWeak.expired());
  CHECK(!runtimeWeak.expired());
  CHECK(!editorWeak.expired());
  CHECK(!clientRuntimeWeak.expired());
  CHECK(!layoutStorageWeak.expired());

  integration->initialize();
  integration->layout().setVisible(StudioPaneId::Settings, true);
  CHECK(integration->layoutSnapshot().isVisible(
      StudioPaneId::Settings));
  CHECK(layoutStorageWeak.lock()->getItem(
            STUDIO_LAYOUT_STORAGE_KEY)
            .has_value());
  integration->relayout();
  integration->poll();
  events->clear();
  integration.reset();

  CHECK(providerWeak.expired());
  CHECK(runtimeWeak.expired());
  CHECK(editorWeak.expired());
  CHECK(clientRuntimeWeak.expired());
  CHECK(layoutStorageWeak.expired());
  const auto editorDispose =
      std::find(events->begin(), events->end(), "editor-dispose");
  const auto runtimeStop =
      std::find(events->begin(), events->end(), "runtime-stop");
  CHECK(editorDispose != events->end());
  CHECK(runtimeStop != events->end());
  CHECK(editorDispose < runtimeStop);
  // Nothing preempted the host: there is no agent to preempt.
  CHECK(std::find(events->begin(), events->end(), "host-clear") ==
        events->end());
}

void testCrowdyClientConstructionHelper() {
  auto crypto = std::make_shared<FakeCrypto>();
  auto transport = std::make_shared<FakeHttpTransport>();
  FakeClock clock;
  FakePlayerHost playerHost;
  std::unique_ptr<CrowdyStudioIntegration> integration;
  int platformPolls = 0;
  {
    ClientConfig config;
    config.httpUrl = "https://game.invalid";
    config.transport = transport;
    config.crypto = crypto.get();
    CrowdyClient client(std::move(config));

    CrowdyStudioIntegrationOptions options;
    options.studio = controllerOptions(clock);
    options.crypto = crypto;
    options.clock = &clock;
    options.playerHost = &playerHost;
    options.platformPoll = [&] {
      ++platformPolls;
      return std::size_t{3};
    };
    integration =
        client.createCrowdyStudioIntegration(std::move(options));
    CHECK(integration);
    CHECK(integration->playerHost() == &playerHost);
    CHECK(dynamic_cast<CrowdyStudioPlayerWalletProvider*>(
              integration->walletProvider()) != nullptr);
    CHECK(integration->poll() >= std::size_t{3});
    CHECK_EQ(platformPolls, 1);
  }
  CHECK(integration->walletProvider() != nullptr);
  (void)integration->tick();
  CHECK_EQ(platformPolls, 2);
  integration->dispose();
}

}  // namespace

int main() {
  testNonblockingPollAndScheduledMaintenance();
  testEditorRoundTripsPollTickAndRelayout();
  testOwnedWalletProviderIsNonfatal();
  testIntegrationApprovedRestoreRequiresInjectedCapabilities();
  testConcreteIntegrationOwnershipAndDestructionOrder();
  testCrowdyClientConstructionHelper();
  std::printf("studio_integration_test passed\n");
  return 0;
}
