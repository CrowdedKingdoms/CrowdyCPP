#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/controller.hpp"
#include "crowdy/studio/diagnostics.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::studio;

namespace {

class CaptureTransport final : public graphql::IHttpTransport {
 public:
  std::deque<graphql::HttpResponse> responses;

  graphql::HttpResponse send(
      const graphql::HttpRequest&) override {
    CHECK(!responses.empty());
    graphql::HttpResponse response = responses.front();
    responses.pop_front();
    return response;
  }
};

std::string fixtureText(std::string_view name) {
  const std::string path =
      std::string(CROWDY_STUDIO_FIXTURE_DIR) + "/" + std::string(name);
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

CrowdyStudioTarget studioTarget(std::string_view value) {
  const auto parsed = targetFromString(value);
  CHECK(parsed.has_value());
  return *parsed;
}

CrowdyStudioDiagnosticSeverity severity(std::string_view value) {
  if (value == "error") return CrowdyStudioDiagnosticSeverity::Error;
  if (value == "warning") return CrowdyStudioDiagnosticSeverity::Warning;
  if (value == "hint") return CrowdyStudioDiagnosticSeverity::Hint;
  return CrowdyStudioDiagnosticSeverity::Info;
}

void checkDiagnostic(const CrowdyStudioDiagnostic& actual,
                     const graphql::Json& expected) {
  CHECK(actual.target == studioTarget(expected["target"].asStringView()));
  CHECK(actual.path == expected["path"].asString());
  CHECK(actual.line ==
        static_cast<std::uint32_t>(expected["line"].asInt64()));
  CHECK(actual.column ==
        static_cast<std::uint32_t>(expected["column"].asInt64()));
  CHECK(actual.severity == severity(expected["severity"].asStringView()));
  CHECK(actual.message == expected["message"].asString());
  CHECK(toString(actual.source) == expected["source"].asStringView());
  if (expected["code"].ok()) {
    CHECK(actual.code ==
          std::optional<std::string>{expected["code"].asString()});
  } else {
    CHECK(!actual.code);
  }
  if (expected["endLine"].ok()) {
    CHECK(actual.endLine ==
          std::optional<std::uint32_t>{static_cast<std::uint32_t>(
              expected["endLine"].asInt64())});
  } else {
    CHECK(!actual.endLine);
  }
  if (expected["endColumn"].ok()) {
    CHECK(actual.endColumn ==
          std::optional<std::uint32_t>{static_cast<std::uint32_t>(
              expected["endColumn"].asInt64())});
  } else {
    CHECK(!actual.endColumn);
  }
}

void testSharedDiagnosticFixture() {
  const graphql::Json fixture = graphql::Json::parse(
      fixtureText("crowdy-studio-diagnostics.v1.json"));
  CHECK(fixture.ok());
  CHECK(fixture["contractVersion"].asString() ==
        "crowdy.studio-diagnostics/1");
  CHECK(fixture["crowdyJs"]["version"].asStringView() == "16.0.0");
  CHECK(fixture["crowdyJs"]["commit"].asStringView() ==
        "fa487142155bbc90a44803cd00599526e48f1853");
  fixture["cases"].forEach([&](const graphql::Json& fixtureCase) {
    const auto parsed = parseRustcDiagnostics(
        fixtureCase["output"].asStringView(),
        studioTarget(fixtureCase["defaultTarget"].asStringView()));
    const graphql::Json expected = fixtureCase["expected"];
    CHECK(parsed.size() == expected.size());
    for (std::size_t index = 0; index < parsed.size(); ++index) {
      checkDiagnostic(parsed[index], expected.at(index));
    }
  });
}

void testParserBoundsAndMalformedInputs() {
  CHECK(parseRustcDiagnostics("", CrowdyStudioTarget::Server).empty());
  CHECK(parseRustcDiagnostics("{not-json",
                              CrowdyStudioTarget::Server)
            .empty());
  CHECK(parseRustcDiagnostics(
            "../secret.rs:1:1: error: escaped",
            CrowdyStudioTarget::Server)
            .empty());
  CHECK(parseRustcDiagnostics(
            "src/lib.rs:0:1: error: zero",
            CrowdyStudioTarget::Server)
            .empty());
  CHECK(parseRustcDiagnostics(
            "src/lib.rs:1000001:1: error: too far",
            CrowdyStudioTarget::Server)
            .empty());

  std::string invalidUtf8 =
      "src/lib.rs:1:1: error: invalid ";
  invalidUtf8.push_back(static_cast<char>(0xff));
  const auto sanitized = parseRustcDiagnostics(
      invalidUtf8, CrowdyStudioTarget::Server);
  CHECK_EQ(sanitized.size(), std::size_t{1});
  CHECK(sanitized[0].message ==
        std::string("invalid \xef\xbf\xbd"));

  std::string longMessage(3'000, 'x');
  const auto truncated = parseRustcDiagnostics(
      "error: " + longMessage + "\n  --> src/lib.rs:1:1",
      CrowdyStudioTarget::Server);
  CHECK_EQ(truncated.size(), std::size_t{1});
  CHECK(truncated[0].message.size() ==
        kCrowdyStudioDiagnosticMaxMessageBytes);

  std::string many;
  for (std::size_t index = 1; index <= 300; ++index) {
    many += "src/lib.rs:" + std::to_string(index) +
            ":1: warning: bounded\n";
  }
  const auto bounded =
      parseRustcDiagnostics(many, CrowdyStudioTarget::Server);
  CHECK(bounded.size() == kCrowdyStudioDiagnosticMaxCount);

  const std::string duplicate =
      "src/lib.rs:2:3: error[E1]: duplicate\n"
      "src/lib.rs:2:3: error[E1]: duplicate";
  CHECK_EQ(parseRustcDiagnostics(
               duplicate, CrowdyStudioTarget::Server)
               .size(),
           std::size_t{1});

  std::string oversized(kCrowdyStudioDiagnosticMaxInputBytes, 'x');
  oversized += "\nsrc/lib.rs:1:1: error: outside input bound";
  CHECK(parseRustcDiagnostics(
            oversized, CrowdyStudioTarget::Server)
            .empty());
}

CrowdyStudioProjectFile sourceFile(std::string content = "fn main() {}") {
  CrowdyStudioProjectFile file;
  file.target = CrowdyStudioTarget::Server;
  file.path = "src/lib.rs";
  file.content = std::move(content);
  file.revision = "1";
  file.createdAt = "2026-07-24T00:00:00Z";
  file.updatedAt = file.createdAt;
  return file;
}

CrowdyStudioProject makeProject() {
  CrowdyStudioProject project;
  project.projectId = "project-1";
  project.appId = "42";
  project.ownerUserId = "7";
  project.gridId = "500";
  project.kind = CrowdyStudioProjectKind::Server;
  project.metadata.name = "Studio state";
  project.metadata.serverModuleName = "state-server";
  project.metadata.pairingPreference =
      CrowdyStudioPairingPreference::None;
  project.files = {sourceFile()};
  project.sdkVersion = "0.1.5";
  project.revision = {"17", "2026-07-24T00:00:00Z"};
  project.fileCount = 1;
  project.totalBytes = "12";
  project.createdAt = "2026-07-24T00:00:00Z";
  project.updatedAt = "2026-07-24T00:00:00Z";
  return project;
}

CrowdyStudioProjectSummary summary(const CrowdyStudioProject& project) {
  return {project.projectId,
          project.gridId,
          project.metadata.name,
          project.kind,
          project.revision.id,
          project.metadata.serverModuleName,
          project.metadata.clientModuleName,
          project.archived,
          project.updatedAt};
}

class FakeProjectProvider final : public ICrowdyStudioProjectProvider {
 public:
  CrowdyStudioProject project = makeProject();
  int saveCalls = 0;

  std::vector<CrowdyStudioProjectSummary> listProjects(
      const CrowdyStudioProjectScope&) override {
    return {summary(project)};
  }

  CrowdyStudioProject getProject(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return project;
  }

  CrowdyStudioProject createProject(
      const CreateCrowdyStudioProjectInput&) override {
    return project;
  }

  CrowdyStudioProject saveProject(
      const SaveCrowdyStudioProjectInput& input) override {
    ++saveCalls;
    CHECK(input.expectedRevisionId == project.revision.id);
    project.metadata = input.metadata;
    project.files = input.files;
    project.revision.id =
        std::to_string(std::stoll(project.revision.id) + 1);
    project.revision.savedAt = "2026-07-24T00:00:01Z";
    project.updatedAt = project.revision.savedAt;
    return project;
  }

  std::vector<CrowdyStudioReferenceFile> listPersonalLibraryFiles(
      const CrowdyStudioProjectScope&) override {
    return {};
  }

  CrowdyStudioReferenceFile savePersonalLibraryFile(
      const SaveCrowdyStudioLibraryFileInput&) override {
    return {};
  }

  std::vector<CrowdyStudioReferenceFile> listCommonFiles(
      const CrowdyStudioProjectScope&) override {
    return {};
  }

  CrowdyStudioProject importReferenceFile(
      const ImportCrowdyStudioReferenceFileInput&) override {
    return project;
  }
};

class FakeRuntime final : public ICrowdyStudioRuntime {
 public:
  int usageCalls = 0;
  std::string compileStatus = "succeeded";
  std::string compileLog;

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput&) override {
    return {"version-1"};
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return {{"version-1", compileStatus, compileLog}};
  }

  void setEnabled(const CrowdyStudioProjectScope&, std::string_view,
                  bool) override {}

  void setRequires(
      const CrowdyStudioProjectScope&, std::string_view,
      const std::optional<std::string>&) override {}

  void startClient(const CrowdyStudioProjectScope&, std::string_view,
                   std::string_view) override {}

  void stopClient() override {}

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope&, std::string_view,
      std::string_view,
      const std::optional<std::string>&) override {
    return {};
  }

  std::optional<CrowdyStudioUsageSnapshot> usage(
      std::string_view) override {
    ++usageCalls;
    return CrowdyStudioUsageSnapshot{
        "3", "9", "100", "500", 1, 20, "active", std::nullopt};
  }
};

class FakeWallet final : public ICrowdyStudioWalletProvider {
 public:
  int calls = 0;
  bool fail = false;

  CrowdyStudioWalletSnapshot balance() override {
    ++calls;
    if (fail) throw std::runtime_error("wallet unavailable");
    return {"250", "USD"};
  }
};

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = 1'784'937'600'000;
  std::int64_t monotonic = 0;

  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
};

CrowdyStudioControllerOptions controllerOptions() {
  CrowdyStudioControllerOptions options;
  options.appId = "42";
  options.gridId = "500";
  options.autosaveMs = 1;
  options.monitorPollMs = 10;
  options.compilePollMs = 0;
  options.sleep = [](std::int64_t) {};
  return options;
}

void testControllerDiagnosticsCompatibilityAndWallet() {
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeWallet wallet;
  FakeClock clock;
  CrowdyStudioController controller(
      controllerOptions(), provider, runtime, core::opensslCrypto(), clock,
      nullptr, nullptr, &wallet);
  controller.initialize();

  controller.setLocalDiagnostics(
      std::vector<std::string>{"legacy advisory"});
  CHECK_EQ(controller.getState().localDiagnostics.size(), std::size_t{1});
  const auto& legacy = controller.getState().localDiagnostics[0];
  CHECK(legacy.path == "src/lib.rs");
  CHECK(legacy.message == "legacy advisory");
  CHECK(legacy.source ==
        CrowdyStudioDiagnosticSource::LocalAdvisory);
  CHECK(controller.localDiagnosticTexts() ==
        std::vector<std::string>{"legacy advisory"});

  CrowdyStudioDiagnostic typed;
  typed.target = CrowdyStudioTarget::Server;
  typed.path = "src/lib.rs";
  typed.line = 4;
  typed.column = 2;
  typed.severity = CrowdyStudioDiagnosticSeverity::Hint;
  typed.message = "typed advisory";
  controller.setLocalDiagnostics(
      std::vector<CrowdyStudioDiagnostic>{typed});
  CHECK(controller.getState().localDiagnostics[0] == typed);
  CHECK(controller.getState().project->revision.id == "17");

  runtime.compileStatus = "failed";
  runtime.compileLog =
      "error[E0308]: mismatched types\n"
      "  --> /build/server/src/lib.rs:6:9";
  const CrowdyStudioDeployResult result = controller.testDraft();
  CHECK(result.status ==
        CrowdyStudioDeployResult::Status::CompileFailed);
  CHECK_EQ(controller.getState().authoritativeDiagnostics.size(),
           std::size_t{1});
  CHECK(controller.getState().authoritativeDiagnostics[0].code ==
        std::optional<std::string>{"E0308"});
  CHECK(controller.authoritativeDiagnosticTexts() ==
        std::vector<std::string>{"mismatched types"});

  runtime.compileStatus = "succeeded";
  runtime.compileLog.clear();
  CHECK(controller.testDraft().status ==
        CrowdyStudioDeployResult::Status::Running);
  CHECK(controller.getState().runtimeSync.startedAt ==
        std::optional<std::string>{"2026-07-25T00:00:00.000Z"});
  CHECK(controller.getState().runtimeSync.startedAtEpochMs ==
        std::optional<std::int64_t>{clock.epoch});
  CHECK_EQ(wallet.calls, 1);
  wallet.calls = 0;

  controller.setSurfaceVisible(CrowdyStudioPolledSurface::Usage, true);
  CHECK_EQ(wallet.calls, 1);
  const std::optional<CrowdyStudioWalletSnapshot> expectedWallet =
      CrowdyStudioWalletSnapshot{"250", "USD"};
  CHECK(controller.getState().wallet == expectedWallet);
  CHECK(controller.getState().usage.has_value());

  controller.setPageVisible(false);
  clock.monotonic = 100;
  controller.tick();
  CHECK_EQ(wallet.calls, 1);

  wallet.fail = true;
  controller.setPageVisible(true);
  controller.tick();
  CHECK_EQ(wallet.calls, 2);
  CHECK(!controller.getState().wallet);
  CHECK(controller.getState().usage.has_value());

  controller.updateFile(CrowdyStudioTarget::Server, "src/lib.rs",
                        "fn still_authors() {}");
  CHECK(controller.saveNow());
  CHECK_EQ(provider.saveCalls, 1);

  // Existing constructor call sites remain source-compatible without a wallet.
  FakeProjectProvider legacyProvider;
  FakeRuntime legacyRuntime;
  CrowdyStudioController legacyController(
      controllerOptions(), legacyProvider, legacyRuntime,
      core::opensslCrypto(), clock);
  legacyController.initialize();
  legacyController.refreshSurface(CrowdyStudioPolledSurface::Usage);
  CHECK(!legacyController.getState().wallet);
}

void testPlayerWalletAdapter() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));
  CrowdyStudioPlayerWalletProvider provider(client.playerWallet());

  transport->responses.push_back(
      {200,
       R"({"data":{"playerWalletBalance":{"balanceCents":"250","currency":"USD"}}})"});
  const CrowdyStudioWalletSnapshot expected{"250", "USD"};
  CHECK(provider.balance() == expected);

  transport->responses.push_back(
      {200, R"({"data":{"playerWalletBalance":{"currency":"USD"}}})"});
  bool incompleteRejected = false;
  try {
    (void)provider.balance();
  } catch (const std::runtime_error&) {
    incompleteRejected = true;
  }
  CHECK(incompleteRejected);
}

std::string digest(std::string_view value) {
  std::uint8_t bytes[32]{};
  CHECK(core::opensslCrypto().sha256(asBytes(value), bytes));
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result = "sha256:";
  for (const std::uint8_t byte : bytes) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

void testCapabilitiesAndCheckpointBridge() {
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeClock clock;
  CrowdyStudioController controller(
      controllerOptions(), provider, runtime, core::opensslCrypto(), clock);
  controller.initialize();

  bool listUnavailable = false;
  try {
    (void)controller.refreshCheckpoints();
  } catch (const CrowdyStudioCapabilityUnavailableError& error) {
    listUnavailable =
        error.capability() ==
        CrowdyStudioSynchronizationCapability::CheckpointList;
  }
  CHECK(listUnavailable);

  CrowdyStudioAtomicPatchInput patch;
  patch.expectedRevisionId =
      controller.getState().project->revision.id;
  patch.changes.push_back(
      {CrowdyStudioTarget::Server, "src/lib.rs",
       CrowdyStudioPatchOperation::Replace, "fn patched() {}",
       digest("fn main() {}")});
  bool patchUnavailable = false;
  try {
    (void)controller.applyAtomicPatch(patch);
  } catch (const CrowdyStudioCapabilityUnavailableError& error) {
    patchUnavailable =
        error.capability() ==
        CrowdyStudioSynchronizationCapability::AtomicPatch;
  }
  CHECK(patchUnavailable);

  bool restoreUnavailable = false;
  try {
    (void)controller.restoreCheckpoint("checkpoint-1", "grant");
  } catch (const CrowdyStudioCapabilityUnavailableError& error) {
    restoreUnavailable =
        error.capability() ==
        CrowdyStudioSynchronizationCapability::ApprovedRestore;
  }
  CHECK(restoreUnavailable);

  // A checkpoint recorded server-side (the harness's pre-write snapshot)
  // arrives as metadata only; the controller keeps it without content.
  CrowdyStudioCheckpointEvent event;
  event.scope = {"42", "500"};
  event.projectId = "project-1";
  event.checkpoint.checkpointId = "checkpoint-1";
  event.checkpoint.projectRevisionId = "17";
  event.checkpoint.contentHash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  event.checkpoint.reason = CrowdyStudioCheckpointMetadata::Reason::AgentWrite;
  event.checkpoint.files.push_back(CrowdyStudioCheckpointFile{
      CrowdyStudioTarget::Server, "src/lib.rs",
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      12});
  event.checkpoint.createdAt = "2026-07-24T00:00:00Z";
  controller.ingestCheckpointEvent(event);
  CHECK_EQ(controller.getState().checkpoints.size(), std::size_t{1});
  CHECK(controller.getState().checkpoints[0].reason ==
        CrowdyStudioCheckpointMetadata::Reason::AgentWrite);

  CrowdyStudioCheckpointEvent wrongScope = event;
  wrongScope.scope.appId = "43";
  bool scopeRejected = false;
  try {
    controller.ingestCheckpointEvent(wrongScope);
  } catch (const std::invalid_argument&) {
    scopeRejected = true;
  }
  CHECK(scopeRejected);

}

}  // namespace

int main() {
  testSharedDiagnosticFixture();
  testParserBoundsAndMalformedInputs();
  testControllerDiagnosticsCompatibilityAndWallet();
  testPlayerWalletAdapter();
  testCapabilitiesAndCheckpointBridge();
  std::printf("crowdy_studio_state_test passed\n");
  return 0;
}
