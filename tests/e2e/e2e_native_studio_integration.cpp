#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

#define LIVE_CHECK(condition)                                                \
  do {                                                                       \
    if (!(condition)) {                                                      \
      throw std::runtime_error(                                              \
          std::string("live check failed: ") + #condition);                 \
    }                                                                        \
  } while (false)

constexpr const char* kCargoToml = R"toml([package]
name = "crowdy-native-studio-e2e"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]
)toml";

studio::CrowdyStudioProjectFile projectFile(
    std::string path, std::string content) {
  studio::CrowdyStudioProjectFile file;
  file.target = studio::CrowdyStudioTarget::Server;
  file.path = std::move(path);
  file.content = std::move(content);
  return file;
}

class LiveEditor final : public studio::ICrowdyStudioEditorAdapter {
 public:
  studio::CrowdyStudioEditorMode mode() const noexcept override {
    return studio::CrowdyStudioEditorMode::Text;
  }

  void setCallbacks(
      studio::CrowdyStudioEditorCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
  }

  void synchronize(
      const studio::CrowdyStudioEditorSnapshot& snapshot) override {
    snapshot_ = snapshot;
  }

  void relayout() override {}

  void dispose() noexcept override {
    disposed_ = true;
    callbacks_ = {};
    snapshot_ = {};
  }

  void edit(studio::CrowdyStudioTarget target, std::string path,
            std::string content) {
    LIVE_CHECK(callbacks_.onProjectFileChange);
    callbacks_.onProjectFileChange(
        target, std::move(path), std::move(content));
  }

  bool disposed() const noexcept { return disposed_; }

 private:
  studio::CrowdyStudioEditorCallbacks callbacks_;
  studio::CrowdyStudioEditorSnapshot snapshot_;
  bool disposed_ = false;
};

bool archiveProject(domains::CrowdyStudioAPI& api, std::string_view appId,
                    std::string_view gridId, std::string_view projectId,
                    std::string_view suffix) noexcept {
  try {
    auto project = api.getProject(
        {std::string(appId), std::string(gridId)}, projectId);
    if (!project.archived) {
      project = api.setProjectArchived({
          .appId = std::string(appId),
          .projectId = std::string(projectId),
          .expectedRevisionId = project.revision.id,
          .archived = true,
          .idempotencyKey =
              "cpp-native-integration-archive-" + std::string(suffix),
      });
    }
    return project.archived;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native Studio cleanup archive failed: %s\n",
                 error.what());
    return false;
  }
}

void stopAndClose(studio::CrowdyStudioIntegration& integration) noexcept {
  try {
    if (integration.studio().getState().project) {
      (void)integration.studio().stopProject();
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native Studio cleanup stop failed: %s\n",
                 error.what());
  }
  integration.dispose();
}

}  // namespace

int main() {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string gridId =
      e2e::envOr("CROWDY_E2E_STUDIO_GRID_ID");
  if (gridId.empty()) {
    std::puts("CROWDY_E2E_STUDIO_GRID_ID missing; skipping");
    return 77;
  }

  auto& game = e2e::ownerGame(cfg);
  auto& api = game.crowdyStudio();
  const std::string suffix = e2e::runSuffix();
  const std::string module = "native_studio_e2e_" + suffix;
  std::string projectId;
  std::shared_ptr<LiveEditor> editor;
  std::unique_ptr<studio::CrowdyStudioIntegration> integration;

  try {
    E2E_SUBTEST("create a disposable project for the integration");
    studio::CreateCrowdyStudioProjectInput create;
    create.appId = cfg.appId;
    create.gridId = gridId;
    create.kind = studio::CrowdyStudioProjectKind::Server;
    create.metadata.name = "CrowdyCPP native integration " + suffix;
    create.metadata.description = "native Studio integration e2e";
    create.metadata.serverModuleName = module;
    create.idempotencyKey =
        "cpp-native-integration-project-" + suffix;
    create.files = {
        projectFile("Cargo.toml", kCargoToml),
        projectFile(
            "src/lib.rs",
            "#[no_mangle]\n"
            "pub extern \"C\" fn init() {}\n\n"
            "pub fn invoke(input: &[u8]) -> Vec<u8> { input.to_vec() }\n"),
    };
    auto created = api.createProject(create);
    projectId = created.projectId;
    LIVE_CHECK(!projectId.empty());

    E2E_SUBTEST("factory initializes editor and layout");
    editor = std::make_shared<LiveEditor>();
    auto layout =
        std::make_shared<studio::InMemoryCrowdyStudioLayoutStorage>();

    studio::CrowdyStudioIntegrationOptions options;
    options.studio.appId = cfg.appId;
    options.studio.gridId = gridId;
    options.studio.initialProjectId = projectId;
    options.studio.autosaveMs = 0;
    options.studio.compilePollMs = 500;
    options.studio.compilePollLimit = 120;
    options.editor = editor;
    options.layoutStorage = layout;

    integration =
        game.createCrowdyStudioIntegration(std::move(options));
    integration->initializeStudio();

    E2E_SUBTEST("edit in memory and save only on maintenance lane");
    const std::string edited =
        "#[no_mangle]\n"
        "pub extern \"C\" fn init() {}\n\n"
        "pub fn invoke(input: &[u8]) -> Vec<u8> {\n"
        "    let mut output = input.to_vec();\n"
        "    output.push(15);\n"
        "    output\n"
        "}\n";
    const std::string beforeRevision =
        integration->studio().getState().project->revision.id;
    editor->edit(
        studio::CrowdyStudioTarget::Server, "src/lib.rs", edited);
    LIVE_CHECK(integration->studio().getState().saveState ==
               studio::CrowdyStudioSaveState::Saving);
    integration->poll();
    LIVE_CHECK(integration->studio().getState().project->revision.id ==
               beforeRevision);
    (void)integration->runStudioMaintenance();
    LIVE_CHECK(integration->studio().getState().saveState ==
               studio::CrowdyStudioSaveState::Saved);
    const std::string savedRevision =
        integration->studio().getState().project->revision.id;
    LIVE_CHECK(savedRevision != beforeRevision);
    const auto saved =
        api.getProject({cfg.appId, gridId}, projectId);
    const auto savedFile = std::find_if(
        saved.files.begin(), saved.files.end(), [](const auto& file) {
          return file.target == studio::CrowdyStudioTarget::Server &&
                 file.path == "src/lib.rs";
        });
    LIVE_CHECK(savedFile != saved.files.end());
    LIVE_CHECK(savedFile->content == edited);

    E2E_SUBTEST("run the exact saved draft through explicit maintenance");
    const auto plan = integration->studio().makeDeploymentPlan();
    LIVE_CHECK(plan.expectedRevisionId == savedRevision);
    const auto draft = integration->studio().testDraftPlan(plan);
    if (draft.status !=
        studio::CrowdyStudioDeployResult::Status::Running) {
      std::fprintf(
          stderr, "native Studio draft failed: %s\n",
          draft.message.c_str());
    }
    const bool admissionPending =
        draft.message.find("awaiting admission") != std::string::npos;
    LIVE_CHECK(
        draft.status ==
            studio::CrowdyStudioDeployResult::Status::Running ||
        admissionPending);
    if (draft.status ==
        studio::CrowdyStudioDeployResult::Status::Running) {
      LIVE_CHECK(integration->studio().getState().runtimeSync.state ==
                 studio::CrowdyStudioRuntimeSyncState::RunningSaved);
    } else {
      std::puts(
          "Draft reached the configured code-admission gate; runtime start "
          "is intentionally pending operator approval");
    }

    E2E_SUBTEST("the agent's policy and usage read models answer");
    // The agent itself runs in the player's browser; what a native client can
    // still read is the app policy, the caller's consent and metered usage.
    if (e2e::envFlag("CROWDY_E2E_AGENT")) {
      auto& agentApi = game.crowdyStudioAgent();
      const auto policy = agentApi.effectivePolicy(cfg.appId);
      LIVE_CHECK(policy.ok());
      const auto consent = agentApi.providerConsent(cfg.appId);
      LIVE_CHECK(consent.ok());
      LIVE_CHECK(consent["appId"].asString() == cfg.appId);
      const auto usage = agentApi.modelUsage(cfg.appId, 5);
      LIVE_CHECK(usage.ok());
      LIVE_CHECK(usage["payerKind"].ok());
      LIVE_CHECK(usage["recent"].ok());
    } else {
      std::puts(
          "CROWDY_E2E_AGENT unset; agent policy/usage read models skipped");
    }

    E2E_SUBTEST("approved restore capability detection");
    if (e2e::envFlag(
            "CROWDY_E2E_STUDIO_APPROVED_RESTORE_CAPABILITY")) {
      throw std::runtime_error(
          "deployment advertised approved restore, but this checkout has "
          "no injected synchronization/approval provider");
    }
    std::puts(
        "No injected synchronization/approval provider advertised; "
        "approved restore live subtest skipped (offline gate covers it)");

    stopAndClose(*integration);
    integration.reset();
    LIVE_CHECK(editor->disposed());
    LIVE_CHECK(archiveProject(
        api, cfg.appId, gridId, projectId, suffix));
    std::puts("e2e_native_studio_integration passed");
    return 0;
  } catch (const graphql::CrowdyGraphQLError& error) {
    e2e::printGraphQLError(
        error, "native Studio integration GraphQL error");
  } catch (const std::exception& error) {
    std::fprintf(
        stderr, "native Studio integration exception: %s\n",
        error.what());
  }

  if (integration) {
    stopAndClose(*integration);
    integration.reset();
  }
  if (!projectId.empty()) {
    (void)archiveProject(
        api, cfg.appId, gridId, projectId, suffix);
  }
  return 1;
}
