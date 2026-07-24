#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/models.hpp"
#include "crowdy/studio/runtime.hpp"

namespace crowdy::studio {

enum class CrowdyStudioSaveState { Saving, Saved, Conflict, Offline };
enum class CrowdyStudioAgentActivity { Idle, Preparing, Working, Paused };
enum class CrowdyStudioPolledSurface { Runs, Logs, Usage };
enum class CrowdyStudioPhase {
  Idle,
  TestingDraft,
  DeployingLive,
  Compiling,
  Enabling,
  Running,
  CompileFailed,
  Stopping,
  Stopped,
  PartialFailure,
  Error,
};
enum class CrowdyStudioRuntimeSyncState {
  NeverRun,
  RunningSaved,
  RunningStale,
  Stopped,
};

struct CrowdyStudioRuntimeStatus {
  CrowdyStudioPhase phase = CrowdyStudioPhase::Idle;
  std::optional<CrowdyStudioTarget> target;
  std::string message;
};

struct CrowdyStudioRuntimeSync {
  CrowdyStudioRuntimeSyncState state =
      CrowdyStudioRuntimeSyncState::NeverRun;
  std::optional<std::string> savedRevisionId;
  std::optional<std::string> runningRevisionId;
  std::optional<CrowdyStudioDeployment> deployment;
  std::optional<std::string> runningProjectContentHash;
  std::optional<std::string> runningServerModuleName;
  std::optional<std::string> runningClientModuleName;
  std::optional<CrowdyStudioPairingPreference> runningPairingPreference;
  std::optional<std::int64_t> startedAtEpochMs;
};

struct CrowdyStudioFileRef {
  enum class Source { Project, PersonalLibrary, Common };

  Source source = Source::Project;
  std::optional<CrowdyStudioTarget> target;
  std::string path;
  std::optional<std::string> referenceId;
};

struct CrowdyStudioState {
  std::vector<CrowdyStudioProjectSummary> projects;
  std::optional<CrowdyStudioProject> project;
  std::vector<CrowdyStudioReferenceFile> personalLibraryFiles;
  std::vector<CrowdyStudioReferenceFile> commonFiles;
  std::vector<CrowdyStudioFileRef> openFiles;
  std::optional<CrowdyStudioFileRef> activeFile;
  CrowdyStudioSaveState saveState = CrowdyStudioSaveState::Saved;
  std::string saveMessage;
  CrowdyStudioRuntimeStatus runtime;
  CrowdyStudioRuntimeSync runtimeSync;
  CrowdyStudioAgentActivity agentActivity =
      CrowdyStudioAgentActivity::Idle;
  std::vector<CrowdyStudioCheckpointMetadata> checkpoints;
  std::string buildOutput;
  std::vector<std::string> authoritativeDiagnostics;
  std::vector<std::string> localDiagnostics;
  std::vector<CrowdyStudioRun> runs;
  std::vector<CrowdyStudioRun> logs;
  std::optional<CrowdyStudioUsageSnapshot> usage;
  std::optional<CrowdyStudioInvokeResult> invokeResult;
};

struct CrowdyStudioAgentWorkContext {
  std::optional<std::string> projectId;
  std::optional<std::string> projectRevisionId;
  CrowdyStudioSaveState saveState = CrowdyStudioSaveState::Saved;
  CrowdyStudioRuntimeSync runtimeSync;
};

struct CrowdyStudioAgentContext {
  std::string appRef;
  std::optional<std::string> projectRef;
  std::string gridRef;
  std::string contextVersion;
  std::optional<std::string> projectContentHash;
};

struct CrowdyStudioDeployResult {
  enum class Status { Running, CompileFailed, Failed };

  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
  Status status = Status::Failed;
  std::string projectRevisionId;
  std::vector<CrowdyStudioTarget> targets;
  std::string message;
};

struct CrowdyStudioStopResult {
  std::optional<bool> serverStopped;
  std::optional<bool> clientStopped;
  std::vector<std::string> failures;
};

struct CrowdyStudioTargetPermission {
  bool canWrite = true;
  bool canRun = true;
};

struct CrowdyStudioSettingsPatch {
  std::optional<std::string> name;
  CrowdyStudioPatchField<std::string> description;
  CrowdyStudioPatchField<std::string> serverModuleName;
  CrowdyStudioPatchField<std::string> clientModuleName;
  std::optional<CrowdyStudioPairingPreference> pairingPreference;
};

struct CrowdyStudioControllerOptions {
  std::string appId;
  std::string gridId;
  std::optional<std::string> initialProjectId;
  std::optional<CrowdyStudioTargetPermission> serverPermission;
  std::optional<CrowdyStudioTargetPermission> clientPermission;
  std::int64_t autosaveMs = 700;
  std::int64_t retryMs = 3'000;
  std::int64_t compilePollMs = 1'500;
  int compilePollLimit = 60;
  std::int64_t monitorPollMs = 5'000;
  std::function<void(std::int64_t)> sleep;
  std::function<bool()> isOnline;
  std::function<void(const CrowdyStudioState&)> onStateChange;
  std::function<void(
      const CrowdyStudioProject&,
      const CrowdyStudioProjectSynchronization&)>
      onProjectSynchronized;
};

class CrowdyStudioController {
 public:
  using ListenerId = std::uint64_t;
  using StateListener = std::function<void(const CrowdyStudioState&)>;
  using HumanEditListener = std::function<void()>;
  using MutableFileIterator =
      std::vector<CrowdyStudioProjectFile>::iterator;
  using ConstFileIterator =
      std::vector<CrowdyStudioProjectFile>::const_iterator;

  CrowdyStudioController(
      CrowdyStudioControllerOptions options,
      ICrowdyStudioProjectProvider& projectProvider,
      ICrowdyStudioRuntime& runtime, const core::ICrypto& crypto,
      const core::IClock& clock = core::systemClock(),
      ICrowdyStudioSynchronizationProvider* synchronizationProvider = nullptr,
      ICrowdyStudioApprovalGate* approvalGate = nullptr)
      : options_(std::move(options)),
        projectProvider_(projectProvider),
        runtime_(runtime),
        crypto_(crypto),
        clock_(clock),
        synchronizationProvider_(synchronizationProvider),
        approvalGate_(approvalGate) {
    if (options_.appId.empty() || options_.gridId.empty()) {
      throw std::invalid_argument(
          "Crowdy Studio requires an app-scoped appId and gridId");
    }
    if (options_.onStateChange) {
      listeners_.emplace(nextListenerId_++, options_.onStateChange);
    }
  }

  const CrowdyStudioState& getState() const { return state_; }

  ListenerId subscribe(StateListener listener) {
    ensureAlive();
    const ListenerId id = nextListenerId_++;
    listeners_.emplace(id, std::move(listener));
    listeners_.at(id)(state_);
    return id;
  }

  void unsubscribe(ListenerId id) { listeners_.erase(id); }

  ListenerId onHumanEdit(HumanEditListener listener) {
    ensureAlive();
    const ListenerId id = nextListenerId_++;
    humanEditListeners_.emplace(id, std::move(listener));
    return id;
  }

  void unsubscribeHumanEdit(ListenerId id) {
    humanEditListeners_.erase(id);
  }

  CrowdyStudioAgentWorkContext prepareForAgentWork() {
    ensureAlive();
    state_.agentActivity = CrowdyStudioAgentActivity::Preparing;
    notify();
    if (!saveNow() || state_.saveState != CrowdyStudioSaveState::Saved) {
      state_.agentActivity = CrowdyStudioAgentActivity::Paused;
      notify();
      throw std::runtime_error(
          "Resolve the project save before starting agent work");
    }
    state_.agentActivity = CrowdyStudioAgentActivity::Working;
    notify();
    CrowdyStudioAgentWorkContext context;
    if (state_.project) {
      context.projectId = state_.project->projectId;
      context.projectRevisionId = state_.project->revision.id;
    }
    context.saveState = CrowdyStudioSaveState::Saved;
    context.runtimeSync = state_.runtimeSync;
    return context;
  }

  void finishAgentWork(bool paused = false) {
    state_.agentActivity = paused ? CrowdyStudioAgentActivity::Paused
                                  : CrowdyStudioAgentActivity::Idle;
    notify();
  }

  std::uint64_t beginAgentOperation() {
    return ++agentOperationGeneration_;
  }

  void cancelAgentOperation(
      std::string message = "Agent operation cancelled") {
    ++agentOperationGeneration_;
    ++operationGeneration_;
    state_.agentActivity = CrowdyStudioAgentActivity::Paused;
    state_.runtime = {
        CrowdyStudioPhase::Idle, std::nullopt, std::move(message)};
    notify();
  }

  bool canTarget(CrowdyStudioTarget target,
                 std::string_view action) const {
    const auto& permission =
        target == CrowdyStudioTarget::Server
            ? options_.serverPermission
            : options_.clientPermission;
    if (!permission) return true;
    return action == "write" ? permission->canWrite : permission->canRun;
  }

  CrowdyStudioAgentContext getAgentContext() const {
    CrowdyStudioAgentContext context;
    context.appRef = options_.appId;
    context.gridRef = options_.gridId;
    graphql::JVal canonical;
    canonical["contract"] = "crowdy.studio-context/1";
    canonical["appRef"] = options_.appId;
    canonical["gridRef"] = options_.gridId;
    canonical["saveState"] = saveStateString(state_.saveState);
    canonical["runtimeSync"] = runtimeSyncInput(state_.runtimeSync);
    if (state_.project) {
      context.projectRef = state_.project->projectId;
      context.projectContentHash = projectContentHash(*state_.project);
      canonical["projectRef"] = state_.project->projectId;
      canonical["projectRevisionId"] = state_.project->revision.id;
      canonical["projectContentHash"] = *context.projectContentHash;
    }
    context.contextVersion = sha256(canonical.dump());
    return context;
  }

  void initialize() {
    ensureAlive();
    try {
      const CrowdyStudioProjectScope projectScope = scope();
      state_.projects = projectProvider_.listProjects(projectScope);
      state_.personalLibraryFiles =
          projectProvider_.listPersonalLibraryFiles(projectScope);
      state_.commonFiles = projectProvider_.listCommonFiles(projectScope);
      state_.saveState = CrowdyStudioSaveState::Saved;
      state_.saveMessage.clear();
      retryDueMs_.reset();
      notify();
      const auto selected = std::find_if(
          state_.projects.begin(), state_.projects.end(),
          [&](const CrowdyStudioProjectSummary& project) {
            return options_.initialProjectId &&
                   project.projectId == *options_.initialProjectId;
          });
      if (selected != state_.projects.end()) {
        loadProject(selected->projectId);
      } else if (!state_.projects.empty()) {
        loadProject(state_.projects.front().projectId);
      }
    } catch (const CrowdyStudioOfflineError& error) {
      state_.saveState = CrowdyStudioSaveState::Offline;
      state_.saveMessage = error.what();
      scheduleRetry();
      notify();
    }
  }

  CrowdyStudioProject createProject(
      const CreateCrowdyStudioProjectInput& input) {
    ensureAlive();
    if (input.appId != options_.appId ||
        (input.gridId && *input.gridId != options_.gridId)) {
      throw std::invalid_argument(
          "Crowdy Studio project creation crossed the controller scope");
    }
    if (state_.project && !saveNow()) {
      throw std::runtime_error(
          "Resolve the current project save before creating another");
    }
    CrowdyStudioProject project = projectProvider_.createProject(input);
    installProject(project);
    upsertSummary(project);
    notify();
    return project;
  }

  void switchProject(std::string_view projectId) {
    ensureAlive();
    if (state_.project && state_.project->projectId == projectId) return;
    if (state_.project && !saveNow()) {
      throw std::runtime_error(
          "Resolve the current project save before switching");
    }
    loadProject(projectId);
  }

  void openFile(const CrowdyStudioFileRef& reference) {
    (void)fileContent(reference);
    const auto found = std::find_if(
        state_.openFiles.begin(), state_.openFiles.end(),
        [&](const CrowdyStudioFileRef& open) {
          return sameFileRef(open, reference);
        });
    if (found == state_.openFiles.end()) {
      state_.openFiles.push_back(reference);
    }
    state_.activeFile = reference;
    notify();
  }

  void closeFile(const CrowdyStudioFileRef& reference) {
    state_.openFiles.erase(
        std::remove_if(
            state_.openFiles.begin(), state_.openFiles.end(),
            [&](const CrowdyStudioFileRef& open) {
              return sameFileRef(open, reference);
            }),
        state_.openFiles.end());
    if (state_.activeFile &&
        sameFileRef(*state_.activeFile, reference)) {
      state_.activeFile =
          state_.openFiles.empty()
              ? std::optional<CrowdyStudioFileRef>{}
              : std::optional<CrowdyStudioFileRef>{state_.openFiles.back()};
    }
    notify();
  }

  const std::string& fileContent(
      const CrowdyStudioFileRef& reference) const {
    if (reference.source == CrowdyStudioFileRef::Source::Project) {
      const CrowdyStudioProject& project = requireProject();
      const std::string path = normalizeCrowdyStudioPath(reference.path);
      const auto found = std::find_if(
          project.files.begin(), project.files.end(),
          [&](const CrowdyStudioProjectFile& file) {
            return reference.target && file.target == *reference.target &&
                   file.path == path;
          });
      if (found != project.files.end()) return found->content;
    } else {
      const auto& files =
          reference.source ==
                  CrowdyStudioFileRef::Source::PersonalLibrary
              ? state_.personalLibraryFiles
              : state_.commonFiles;
      const auto found = std::find_if(
          files.begin(), files.end(),
          [&](const CrowdyStudioReferenceFile& file) {
            if (reference.referenceId) {
              return file.id == *reference.referenceId;
            }
            return reference.target && file.target == *reference.target &&
                   file.path == reference.path;
          });
      if (found != files.end()) return found->content;
    }
    throw std::runtime_error("Crowdy Studio file is not loaded");
  }

  void addFile(CrowdyStudioTarget target, std::string_view path,
               std::string content = {}) {
    CrowdyStudioProject& project = requireWritableProject();
    assertProjectTarget(project, target);
    assertTargetWritable(target);
    const std::string normalized = normalizeCrowdyStudioPath(path);
    if (findFile(project, target, normalized) != project.files.end()) {
      throw std::invalid_argument("Crowdy Studio project file already exists");
    }
    CrowdyStudioProjectFile file;
    file.target = target;
    file.path = normalized;
    file.content = std::move(content);
    project.files.push_back(std::move(file));
    sortFiles(project.files);
    markEdited();
    openFile(projectFileRef(target, normalized));
  }

  void renameFile(CrowdyStudioTarget target, std::string_view path,
                  std::string_view nextPath) {
    CrowdyStudioProject& project = requireWritableProject();
    assertTargetWritable(target);
    const std::string normalized = normalizeCrowdyStudioPath(path);
    const std::string renamed = normalizeCrowdyStudioPath(nextPath);
    auto file = findFile(project, target, normalized);
    if (file == project.files.end()) {
      throw std::invalid_argument("Crowdy Studio project file does not exist");
    }
    if (findFile(project, target, renamed) != project.files.end()) {
      throw std::invalid_argument("Crowdy Studio destination already exists");
    }
    file->path = renamed;
    for (auto& reference : state_.openFiles) {
      replaceFileRef(reference, target, normalized, renamed);
    }
    if (state_.activeFile) {
      replaceFileRef(*state_.activeFile, target, normalized, renamed);
    }
    sortFiles(project.files);
    markEdited();
  }

  void deleteFile(CrowdyStudioTarget target, std::string_view path) {
    CrowdyStudioProject& project = requireWritableProject();
    assertTargetWritable(target);
    const std::string normalized = normalizeCrowdyStudioPath(path);
    const auto file = findFile(project, target, normalized);
    if (file == project.files.end()) {
      throw std::invalid_argument("Crowdy Studio project file does not exist");
    }
    project.files.erase(file);
    closeFile(projectFileRef(target, normalized));
    markEdited();
  }

  void updateFile(CrowdyStudioTarget target, std::string_view path,
                  std::string content) {
    CrowdyStudioProject& project = requireWritableProject();
    assertTargetWritable(target);
    const std::string normalized = normalizeCrowdyStudioPath(path);
    const auto file = findFile(project, target, normalized);
    if (file == project.files.end()) {
      throw std::invalid_argument("Crowdy Studio project file does not exist");
    }
    if (file->content == content) return;
    file->content = std::move(content);
    markEdited();
  }

  void updateSettings(const CrowdyStudioSettingsPatch& patch) {
    CrowdyStudioProject& project = requireWritableProject();
    if (patch.name) {
      if (patch.name->empty()) {
        throw std::invalid_argument("Crowdy Studio project name is required");
      }
      project.metadata.name = *patch.name;
    }
    applyPatchField(project.metadata.description, patch.description);
    applyPatchField(project.metadata.serverModuleName,
                    patch.serverModuleName);
    applyPatchField(project.metadata.clientModuleName,
                    patch.clientModuleName);
    if (patch.pairingPreference) {
      validatePairing(project.kind, *patch.pairingPreference);
      project.metadata.pairingPreference = *patch.pairingPreference;
    }
    markEdited();
  }

  void setPairingPreference(CrowdyStudioPairingPreference preference) {
    CrowdyStudioSettingsPatch patch;
    patch.pairingPreference = preference;
    updateSettings(patch);
  }

  void setLocalDiagnostics(std::vector<std::string> diagnostics) {
    state_.localDiagnostics = std::move(diagnostics);
    notify();
  }

  bool saveNow() {
    ensureAlive();
    autosaveDueMs_.reset();
    if (!state_.project) return true;
    if (persistedGeneration_ == editGeneration_) {
      state_.saveState = CrowdyStudioSaveState::Saved;
      state_.saveMessage.clear();
      notify();
      return true;
    }
    const std::uint64_t savingGeneration = editGeneration_;
    const CrowdyStudioProject snapshot = *state_.project;
    state_.saveState = CrowdyStudioSaveState::Saving;
    state_.saveMessage.clear();
    notify();
    try {
      CrowdyStudioProject saved = projectProvider_.saveProject({
          options_.appId,
          options_.gridId,
          snapshot.projectId,
          snapshot.revision.id,
          snapshot.metadata,
          snapshot.files,
          snapshot.sdkVersion,
          snapshot.abiVersion,
          std::nullopt,
      });
      validateProjectScope(saved);
      persistedGeneration_ = savingGeneration;
      if (state_.project &&
          state_.project->projectId == saved.projectId) {
        if (editGeneration_ == savingGeneration) {
          state_.project = saved;
        } else {
          state_.project->revision = saved.revision;
          state_.project->updatedAt = saved.updatedAt;
        }
      }
      upsertSummary(saved);
      state_.saveState =
          persistedGeneration_ == editGeneration_
              ? CrowdyStudioSaveState::Saved
              : CrowdyStudioSaveState::Saving;
      state_.saveMessage.clear();
      conflictRemote_.reset();
      state_.runtimeSync.savedRevisionId = saved.revision.id;
      reconcileRuntimeSync(saved.revision.id);
      notify();
      if (persistedGeneration_ != editGeneration_) return saveNow();
      return true;
    } catch (const CrowdyStudioRevisionConflictError& error) {
      conflictRemote_ = error.remoteProject();
      state_.saveState = CrowdyStudioSaveState::Conflict;
      state_.saveMessage = error.what();
      notify();
      return false;
    } catch (const CrowdyStudioOfflineError& error) {
      state_.saveState = CrowdyStudioSaveState::Offline;
      state_.saveMessage = error.what();
      scheduleRetry();
      notify();
      return false;
    } catch (...) {
      state_.saveState = CrowdyStudioSaveState::Offline;
      state_.saveMessage = "Crowdy Studio project save failed";
      notify();
      throw;
    }
  }

  bool retrySave() {
    ensureAlive();
    retryDueMs_.reset();
    if (!state_.project) {
      state_.saveState = CrowdyStudioSaveState::Saving;
      notify();
      initialize();
      return state_.saveState != CrowdyStudioSaveState::Offline;
    }
    if (state_.saveState == CrowdyStudioSaveState::Conflict) return false;
    return saveNow();
  }

  void acceptRemoteConflict() {
    if (state_.saveState != CrowdyStudioSaveState::Conflict) return;
    CrowdyStudioProject remote =
        conflictRemote_
            ? *conflictRemote_
            : projectProvider_.getProject(scope(), requireProject().projectId);
    installProject(remote);
  }

  bool overwriteConflict() {
    if (state_.saveState != CrowdyStudioSaveState::Conflict) {
      return saveNow();
    }
    CrowdyStudioProject& project = requireProject();
    const CrowdyStudioProject remote =
        conflictRemote_
            ? *conflictRemote_
            : projectProvider_.getProject(scope(), project.projectId);
    validateProjectScope(remote);
    project.revision = remote.revision;
    conflictRemote_.reset();
    state_.saveState = CrowdyStudioSaveState::Saving;
    notify();
    return saveNow();
  }

  void importReferenceFile(const CrowdyStudioReferenceFile& reference,
                           std::string destinationPath = {}) {
    if (!saveNow()) {
      throw std::runtime_error(
          "Resolve the current project save before importing a file");
    }
    const CrowdyStudioProject current = requireProject();
    if (!canTarget(reference.target, "write")) {
      throw std::runtime_error(
          "Crowdy Studio target authoring is unavailable");
    }
    if (destinationPath.empty()) destinationPath = reference.path;
    CrowdyStudioProject saved = projectProvider_.importReferenceFile({
        options_.appId,
        options_.gridId,
        current.projectId,
        current.revision.id,
        reference.source,
        reference.id,
        normalizeCrowdyStudioPath(destinationPath),
        std::nullopt,
    });
    installProject(saved);
    upsertSummary(saved);
    const auto file = findFile(saved, reference.target, destinationPath);
    if (file != saved.files.end()) {
      openFile(projectFileRef(file->target, file->path));
    }
  }

  CrowdyStudioReferenceFile saveProjectFileToLibrary(
      CrowdyStudioTarget target, std::string_view path,
      std::string title = {}) {
    const CrowdyStudioProject& project = requireProject();
    const std::string normalized = normalizeCrowdyStudioPath(path);
    const auto file = findFile(project, target, normalized);
    if (file == project.files.end()) {
      throw std::invalid_argument("Crowdy Studio project file does not exist");
    }
    if (title.empty()) {
      const std::size_t slash = normalized.rfind('/');
      title = slash == std::string::npos ? normalized
                                         : normalized.substr(slash + 1);
    }
    CrowdyStudioReferenceFile saved =
        projectProvider_.savePersonalLibraryFile({
            options_.appId,
            std::nullopt,
            std::nullopt,
            std::move(title),
            target,
            normalized,
            file->content,
            {},
            std::nullopt,
        });
    upsertReference(state_.personalLibraryFiles, saved);
    notify();
    return saved;
  }

  std::vector<CrowdyStudioCheckpointMetadata> refreshCheckpoints() {
    const CrowdyStudioProject& project = requireProject();
    if (synchronizationProvider_) {
      state_.checkpoints = synchronizationProvider_->listCheckpoints(
          scope(), project.projectId);
      notify();
    }
    return state_.checkpoints;
  }

  CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioAtomicPatchInput& input) {
    if (!saveNow()) {
      throw std::runtime_error(
          "Resolve the current project save before applying an agent patch");
    }
    const CrowdyStudioProject baseline = requireProject();
    if (baseline.revision.id != input.expectedRevisionId) {
      throw CrowdyStudioRevisionConflictError(
          "Atomic patch expected a different project revision", baseline);
    }
    validateAtomicPatch(baseline, input);
    if (!synchronizationProvider_) {
      throw std::runtime_error(
          "Atomic patches require a durable synchronization provider");
    }
    CrowdyStudioAtomicPatchResult result =
        synchronizationProvider_->applyAtomicPatch(
            scope(), baseline.projectId, input);
    validateProjectScope(result.project);
    if (result.project.projectId != baseline.projectId ||
        result.project.revision.id == baseline.revision.id ||
        result.checkpoint.projectRevisionId != baseline.revision.id) {
      throw std::runtime_error(
          "Atomic patch returned invalid revision/checkpoint metadata");
    }
    for (const auto& change : input.changes) {
      const auto file =
          findFile(result.project, change.target, change.path);
      if (file == result.project.files.end() ||
          file->content != change.content) {
        throw std::runtime_error(
            "Atomic patch did not synchronize every requested file");
      }
    }
    synchronizeProject(
        result.project,
        {CrowdyStudioProjectSynchronization::Source::Agent,
         baseline.revision.id, result.checkpoint});
    return result;
  }

  void synchronizeProject(
      const CrowdyStudioProject& project,
      const CrowdyStudioProjectSynchronization& synchronization) {
    const CrowdyStudioProject& current = requireProject();
    validateProjectScope(project);
    if (project.projectId != current.projectId) {
      throw std::invalid_argument(
          "Project synchronization target does not match the open project");
    }
    if (synchronization.expectedPreviousRevisionId &&
        *synchronization.expectedPreviousRevisionId !=
            current.revision.id) {
      throw CrowdyStudioRevisionConflictError(
          "Project synchronization started from a stale revision", project);
    }
    if (persistedGeneration_ != editGeneration_) {
      conflictRemote_ = project;
      state_.saveState = CrowdyStudioSaveState::Conflict;
      state_.saveMessage =
          "Human edits preempted an incoming project revision";
      state_.agentActivity = CrowdyStudioAgentActivity::Paused;
      notify();
      throw CrowdyStudioRevisionConflictError(
          "Human edits preempted project synchronization", project);
    }

    state_.project = project;
    editGeneration_ = 0;
    persistedGeneration_ = 0;
    conflictRemote_.reset();
    autosaveDueMs_.reset();
    retryDueMs_.reset();
    state_.openFiles.erase(
        std::remove_if(
            state_.openFiles.begin(), state_.openFiles.end(),
            [&](const CrowdyStudioFileRef& reference) {
              return !fileRefExists(reference);
            }),
        state_.openFiles.end());
    if (state_.activeFile && !fileRefExists(*state_.activeFile)) {
      state_.activeFile =
          state_.openFiles.empty()
              ? std::optional<CrowdyStudioFileRef>{}
              : std::optional<CrowdyStudioFileRef>{state_.openFiles.back()};
    }
    state_.saveState = CrowdyStudioSaveState::Saved;
    state_.saveMessage.clear();
    state_.runtimeSync.savedRevisionId = project.revision.id;
    reconcileRuntimeSync(project.revision.id);
    upsertSummary(project);
    if (synchronization.checkpoint) {
      upsertCheckpoint(*synchronization.checkpoint);
    }
    notify();
    if (options_.onProjectSynchronized) {
      options_.onProjectSynchronized(project, synchronization);
    }
  }

  CrowdyStudioCheckpointMetadata restoreCheckpoint(
      std::string_view checkpointId, std::string_view approvalGrant,
      std::optional<std::string> expectedRevisionId = std::nullopt) {
    if (!saveNow()) {
      throw std::runtime_error(
          "Resolve the current project save before restoring a checkpoint");
    }
    if (!synchronizationProvider_ || !approvalGate_) {
      throw std::runtime_error(
          "Checkpoint restore requires synchronization and agent approval");
    }
    const CrowdyStudioProject current = requireProject();
    const std::string expected =
        expectedRevisionId.value_or(current.revision.id);
    if (expected != current.revision.id) {
      throw CrowdyStudioRevisionConflictError(
          "Checkpoint restore expected a different revision", current);
    }
    approvalGate_->requireRestoreApproval(
        {scope(), current.projectId, std::string(checkpointId), expected},
        approvalGrant);
    CrowdyStudioCheckpointRestoreResult restored =
        synchronizationProvider_->restoreCheckpoint(
            {scope(), current.projectId, std::string(checkpointId), expected,
             std::string(approvalGrant)});
    validateProjectScope(restored.project);
    if (restored.project.projectId != current.projectId ||
        restored.project.revision.id == current.revision.id ||
        restored.preRestoreCheckpoint.projectRevisionId !=
            current.revision.id) {
      throw std::runtime_error(
          "Checkpoint restore returned invalid synchronization metadata");
    }
    synchronizeProject(
        restored.project,
        {CrowdyStudioProjectSynchronization::Source::Agent, expected,
         restored.preRestoreCheckpoint});
    return restored.preRestoreCheckpoint;
  }

  CrowdyStudioDeploymentPlan makeDeploymentPlan() const {
    const CrowdyStudioProject& project = requireProject();
    return {project.revision.id, projectTargets(project.kind),
            project.metadata.pairingPreference,
            projectContentHash(project)};
  }

  CrowdyStudioDeployResult testDraft(
      std::optional<std::uint64_t> agentOperation = std::nullopt) {
    return testDraftPlan(makeDeploymentPlan(), agentOperation);
  }

  CrowdyStudioDeployResult testDraftPlan(
      const CrowdyStudioDeploymentPlan& plan,
      std::optional<std::uint64_t> agentOperation = std::nullopt) {
    return deployProject(CrowdyStudioDeployment::Draft, plan, {},
                         agentOperation);
  }

  CrowdyStudioDeployResult deployLive(
      const CrowdyStudioDeploymentPlan& plan,
      std::string_view approvalGrant,
      std::optional<std::uint64_t> agentOperation = std::nullopt) {
    return deployLivePlan(plan, approvalGrant, agentOperation);
  }

  CrowdyStudioDeployResult deployLivePlan(
      const CrowdyStudioDeploymentPlan& plan,
      std::string_view approvalGrant,
      std::optional<std::uint64_t> agentOperation = std::nullopt) {
    return deployProject(CrowdyStudioDeployment::Live, plan, approvalGrant,
                         agentOperation);
  }

  CrowdyStudioStopResult stopProject() {
    const CrowdyStudioProject& project = requireProject();
    ++operationGeneration_;
    state_.runtime = {CrowdyStudioPhase::Stopping, std::nullopt, {}};
    notify();
    CrowdyStudioStopResult result;
    const auto targets = projectTargets(project.kind);
    if (containsTarget(targets, CrowdyStudioTarget::Client)) {
      result.clientStopped = false;
      try {
        runtime_.stopClient();
        result.clientStopped = true;
      } catch (const std::exception& error) {
        result.failures.push_back(std::string("Client: ") + error.what());
      }
    }
    if (containsTarget(targets, CrowdyStudioTarget::Server)) {
      result.serverStopped = false;
      try {
        const std::string serverName =
            state_.runtimeSync.runningServerModuleName
                ? *state_.runtimeSync.runningServerModuleName
                : moduleName(project, CrowdyStudioTarget::Server);
        runtime_.setEnabled(scope(),
                            serverName, false);
        result.serverStopped = true;
      } catch (const std::exception& error) {
        result.failures.push_back(std::string("Server: ") + error.what());
      }
    }
    state_.runtime =
        result.failures.empty()
            ? CrowdyStudioRuntimeStatus{CrowdyStudioPhase::Stopped,
                                        std::nullopt, "Project stopped"}
            : CrowdyStudioRuntimeStatus{
                  CrowdyStudioPhase::PartialFailure, std::nullopt,
                  joinFailures(result.failures)};
    state_.runtimeSync.state = CrowdyStudioRuntimeSyncState::Stopped;
    notify();
    return result;
  }

  CrowdyStudioInvokeResult invoke(
      std::string_view exportName,
      std::optional<std::string> paramsJson = std::nullopt,
      std::optional<CrowdyStudioDeployment> expectedDeployment =
          std::nullopt,
      std::optional<std::uint64_t> agentOperation = std::nullopt) {
    checkAgentOperation(agentOperation);
    const CrowdyStudioProject& project = requireProject();
    if (!containsTarget(projectTargets(project.kind),
                        CrowdyStudioTarget::Server)) {
      throw std::runtime_error("Invoke requires a SERVER project target");
    }
    if ((state_.runtimeSync.state !=
             CrowdyStudioRuntimeSyncState::RunningSaved &&
         state_.runtimeSync.state !=
             CrowdyStudioRuntimeSyncState::RunningStale) ||
        !state_.runtimeSync.deployment ||
        !state_.runtimeSync.runningServerModuleName) {
      throw std::runtime_error(
          "Invoke requires a running DRAFT or LIVE environment");
    }
    if (expectedDeployment &&
        *expectedDeployment != *state_.runtimeSync.deployment) {
      throw std::runtime_error(
          "Invoke environment does not match the approved deployment");
    }
    std::string selected(exportName);
    trim(selected);
    if (selected.empty()) selected = "invoke";
    CrowdyStudioInvokeResult result = runtime_.invoke(
        scope(), *state_.runtimeSync.runningServerModuleName, selected,
        paramsJson);
    checkAgentOperation(agentOperation);
    state_.invokeResult = result;
    notify();
    return result;
  }

  void setSurfaceVisible(CrowdyStudioPolledSurface surface, bool visible) {
    surfaceVisible_[surfaceIndex(surface)] = visible;
    if (visible && pageVisible_ && state_.project) {
      try {
        refreshSurface(surface);
      } catch (...) {
      }
      monitorDueMs_ = clock_.monotonicMillis() + options_.monitorPollMs;
    }
  }

  void setPageVisible(bool visible) {
    pageVisible_ = visible;
    if (visible) {
      monitorDueMs_ = clock_.monotonicMillis();
    } else {
      monitorDueMs_.reset();
    }
  }

  void refreshSurface(CrowdyStudioPolledSurface surface) {
    if (!state_.project) return;
    const std::string serverName =
        state_.project->metadata.serverModuleName.value_or("");
    if (surface == CrowdyStudioPolledSurface::Runs) {
      state_.runs = runtime_.runs(scope(), serverName);
    } else if (surface == CrowdyStudioPolledSurface::Logs) {
      state_.logs = runtime_.logs(scope(), serverName);
    } else {
      state_.usage = runtime_.usage(options_.appId);
    }
    notify();
  }

  /// Native autosave/retry/poll pump. Engines call this from their normal tick;
  /// CrowdyCPP never creates a UI or background controller thread.
  void tick() {
    ensureAlive();
    const std::int64_t now = clock_.monotonicMillis();
    if (autosaveDueMs_ && now >= *autosaveDueMs_) {
      try {
        (void)saveNow();
      } catch (...) {
      }
    }
    if (retryDueMs_ && now >= *retryDueMs_ &&
        (!options_.isOnline || options_.isOnline())) {
      try {
        (void)retrySave();
      } catch (...) {
      }
    }
    if (pageVisible_ && monitorDueMs_ && now >= *monitorDueMs_) {
      for (std::size_t index = 0; index < surfaceVisible_.size(); ++index) {
        if (!surfaceVisible_[index]) continue;
        try {
          refreshSurface(static_cast<CrowdyStudioPolledSurface>(index));
        } catch (...) {
        }
      }
      monitorDueMs_ = now + options_.monitorPollMs;
    }
  }

  void destroy() {
    if (destroyed_) return;
    destroyed_ = true;
    ++operationGeneration_;
    ++agentOperationGeneration_;
    autosaveDueMs_.reset();
    retryDueMs_.reset();
    monitorDueMs_.reset();
    try {
      runtime_.stopClient();
    } catch (...) {
    }
    listeners_.clear();
    humanEditListeners_.clear();
  }

 private:
  class OperationCancelledError : public std::runtime_error {
   public:
    OperationCancelledError() : std::runtime_error("Operation cancelled") {}
  };

  struct CompiledTarget {
    CrowdyStudioTarget target = CrowdyStudioTarget::Server;
    std::string name;
    std::string versionId;
  };

  void loadProject(std::string_view projectId) {
    CrowdyStudioProject project =
        projectProvider_.getProject(scope(), projectId);
    installProject(project);
    if (synchronizationProvider_) {
      (void)refreshCheckpoints();
    }
  }

  void installProject(const CrowdyStudioProject& project) {
    validateProjectScope(project);
    ++operationGeneration_;
    try {
      runtime_.stopClient();
    } catch (...) {
    }
    autosaveDueMs_.reset();
    retryDueMs_.reset();
    editGeneration_ = 0;
    persistedGeneration_ = 0;
    conflictRemote_.reset();
    state_.project = project;
    const auto preferred = std::find_if(
        project.files.begin(), project.files.end(),
        [](const CrowdyStudioProjectFile& file) {
          return file.path == "src/lib.rs";
        });
    const auto selected =
        preferred != project.files.end() ? preferred : project.files.begin();
    state_.openFiles.clear();
    state_.activeFile.reset();
    if (selected != project.files.end()) {
      state_.activeFile = projectFileRef(selected->target, selected->path);
      state_.openFiles.push_back(*state_.activeFile);
    }
    state_.saveState = CrowdyStudioSaveState::Saved;
    state_.saveMessage.clear();
    state_.runtime = {};
    state_.runtimeSync = {};
    state_.runtimeSync.state =
        CrowdyStudioRuntimeSyncState::NeverRun;
    state_.runtimeSync.savedRevisionId = project.revision.id;
    state_.agentActivity = CrowdyStudioAgentActivity::Idle;
    state_.checkpoints.clear();
    state_.buildOutput.clear();
    state_.authoritativeDiagnostics.clear();
    state_.localDiagnostics.clear();
    state_.runs.clear();
    state_.logs.clear();
    state_.usage.reset();
    state_.invokeResult.reset();
    notify();
  }

  CrowdyStudioDeployResult deployProject(
      CrowdyStudioDeployment deployment,
      const CrowdyStudioDeploymentPlan& plan,
      std::string_view approvalGrant,
      std::optional<std::uint64_t> agentOperation) {
    checkAgentOperation(agentOperation);
    if (!saveNow()) {
      return failedDeployment(deployment, plan,
                              "Project must be saved before it can run");
    }
    checkAgentOperation(agentOperation);
    const CrowdyStudioProject project = requireProject();
    assertDeploymentPlan(project, plan, deployment);
    if (deployment == CrowdyStudioDeployment::Live) {
      if (!approvalGate_ || !plan.projectContentHash ||
          !plan.pairingPreference) {
        throw std::runtime_error(
            "Live deployment requires exact agent-layer approval");
      }
      approvalGate_->requireLiveApproval(
          {scope(), project.projectId, project.revision.id,
           *plan.projectContentHash, plan.targets,
           *plan.pairingPreference},
          approvalGrant);
    }
    const std::uint64_t operation = ++operationGeneration_;
    state_.runtime = {
        deployment == CrowdyStudioDeployment::Draft
            ? CrowdyStudioPhase::TestingDraft
            : CrowdyStudioPhase::DeployingLive,
        std::nullopt, {}};
    state_.buildOutput.clear();
    state_.authoritativeDiagnostics.clear();
    notify();
    try {
      if (plan.targets.size() == 1) {
        const auto compiled =
            compileTarget(project, plan.targets.front(), deployment, operation);
        if (!compiled) {
          return compileFailedDeployment(deployment, project, plan.targets);
        }
        if (compiled->target == CrowdyStudioTarget::Server) {
          enableServer(compiled->name, operation);
        } else {
          runClient(*compiled, operation);
        }
      } else {
        const auto client = compileTarget(
            project, CrowdyStudioTarget::Client, deployment, operation);
        if (!client) {
          return compileFailedDeployment(deployment, project, plan.targets);
        }
        const auto server = compileTarget(
            project, CrowdyStudioTarget::Server, deployment, operation);
        if (!server) {
          return compileFailedDeployment(deployment, project, plan.targets);
        }
        checkOperation(operation);
        const std::optional<std::string> requiredClient =
            project.metadata.pairingPreference ==
                    CrowdyStudioPairingPreference::Required
                ? std::optional<std::string>{client->name}
                : std::nullopt;
        runtime_.setRequires(scope(), server->name, requiredClient);
        checkOperation(operation);
        enableServer(server->name, operation);
        runClient(*client, operation);
      }
      const std::string message =
          deployment == CrowdyStudioDeployment::Draft
              ? "Draft test is running"
              : "Project is live";
      state_.runtime = {
          CrowdyStudioPhase::Running, std::nullopt, message};
      state_.runtimeSync = {};
      state_.runtimeSync.state =
          CrowdyStudioRuntimeSyncState::RunningSaved;
      state_.runtimeSync.savedRevisionId = project.revision.id;
      state_.runtimeSync.runningRevisionId = project.revision.id;
      state_.runtimeSync.deployment = deployment;
      state_.runtimeSync.runningProjectContentHash =
          projectContentHash(project);
      state_.runtimeSync.runningServerModuleName =
          project.metadata.serverModuleName;
      state_.runtimeSync.runningClientModuleName =
          project.metadata.clientModuleName;
      state_.runtimeSync.runningPairingPreference =
          project.metadata.pairingPreference;
      state_.runtimeSync.startedAtEpochMs = clock_.epochMillis();
      notify();
      try {
        refreshSurface(CrowdyStudioPolledSurface::Usage);
      } catch (...) {
      }
      return {deployment, CrowdyStudioDeployResult::Status::Running,
              project.revision.id, plan.targets, message};
    } catch (const OperationCancelledError&) {
      return failedDeployment(deployment, plan, "Deployment was cancelled");
    } catch (const std::exception& error) {
      state_.runtime = {
          CrowdyStudioPhase::Error, std::nullopt, error.what()};
      notify();
      return failedDeployment(deployment, plan, error.what());
    }
  }

  std::optional<CompiledTarget> compileTarget(
      const CrowdyStudioProject& project, CrowdyStudioTarget target,
      CrowdyStudioDeployment deployment, std::uint64_t operation) {
    assertTargetWritable(target);
    const std::string name = moduleName(project, target);
    std::vector<CrowdyStudioProjectFile> files;
    for (const auto& file : project.files) {
      if (file.target == target) files.push_back(file);
    }
    if (files.empty()) {
      throw std::runtime_error("Crowdy Studio target has no project files");
    }
    state_.runtime = {CrowdyStudioPhase::Compiling, target,
                      "Submitting " + name};
    notify();
    const CrowdyStudioDeploySubmission submitted = runtime_.deploy(
        {scope(), target, name, files, project.sdkVersion,
         project.abiVersion, deployment});
    checkOperation(operation);
    for (int attempt = 0; attempt < options_.compilePollLimit; ++attempt) {
      const auto versions = runtime_.versions(scope(), name);
      checkOperation(operation);
      const auto version = std::find_if(
          versions.begin(), versions.end(),
          [&](const CrowdyStudioRuntimeVersion& candidate) {
            return candidate.versionId == submitted.versionId;
          });
      if (version != versions.end()) {
        std::string status = version->compileStatus;
        std::transform(status.begin(), status.end(), status.begin(),
                       [](unsigned char value) {
                         return static_cast<char>(std::tolower(value));
                       });
        if (status == "succeeded" || status == "success") {
          recordBuild(target, version->compileLog.value_or(""));
          return CompiledTarget{target, name, submitted.versionId};
        }
        if (status == "failed" || status == "error") {
          recordBuild(target, version->compileLog.value_or(
                                  "Compilation failed without output"));
          state_.runtime = {CrowdyStudioPhase::CompileFailed, target,
                            name + " failed to compile"};
          notify();
          return std::nullopt;
        }
      }
      sleep(options_.compilePollMs);
      checkOperation(operation);
    }
    const std::string message =
        "Compilation timed out after " +
        std::to_string(options_.compilePollLimit) + " polls";
    recordBuild(target, message);
    state_.runtime = {
        CrowdyStudioPhase::CompileFailed, target, message};
    notify();
    return std::nullopt;
  }

  void enableServer(std::string_view name, std::uint64_t operation) {
    if (!canTarget(CrowdyStudioTarget::Server, "run")) {
      throw std::runtime_error(
          "run_server_code is unavailable on this grid");
    }
    state_.runtime = {
        CrowdyStudioPhase::Enabling, CrowdyStudioTarget::Server,
        "Enabling " + std::string(name)};
    notify();
    runtime_.setEnabled(scope(), name, true);
    checkOperation(operation);
  }

  void runClient(const CompiledTarget& compiled,
                 std::uint64_t operation) {
    if (!canTarget(CrowdyStudioTarget::Client, "run")) {
      throw std::runtime_error(
          "run_client_code is unavailable on this grid");
    }
    runtime_.startClient(scope(), compiled.name, compiled.versionId);
    checkOperation(operation);
  }

  void assertDeploymentPlan(
      const CrowdyStudioProject& project,
      const CrowdyStudioDeploymentPlan& plan,
      CrowdyStudioDeployment deployment) const {
    if (project.revision.id != plan.expectedRevisionId) {
      throw CrowdyStudioRevisionConflictError(
          "Deployment plan expected a different project revision", project);
    }
    auto expected = projectTargets(project.kind);
    auto requested = plan.targets;
    std::sort(expected.begin(), expected.end());
    std::sort(requested.begin(), requested.end());
    requested.erase(std::unique(requested.begin(), requested.end()),
                    requested.end());
    if (requested != expected) {
      throw std::runtime_error(
          "Deployment targets must exactly match the full project");
    }
    if (plan.pairingPreference &&
        *plan.pairingPreference != project.metadata.pairingPreference) {
      throw std::runtime_error(
          "Deployment pairing preference changed after approval");
    }
    const std::string contentHash = projectContentHash(project);
    if (plan.projectContentHash &&
        *plan.projectContentHash != contentHash) {
      throw std::runtime_error(
          "Deployment project content changed after approval");
    }
    if (deployment == CrowdyStudioDeployment::Live &&
        (!plan.pairingPreference || !plan.projectContentHash)) {
      throw std::runtime_error(
          "Live deployment requires exact pairing and content bindings");
    }
  }

  void validateAtomicPatch(
      const CrowdyStudioProject& baseline,
      const CrowdyStudioAtomicPatchInput& input) const {
    if (input.changes.empty() || input.changes.size() > 16) {
      throw std::invalid_argument(
          "Atomic patch must contain 1 through 16 file changes");
    }
    CrowdyStudioProject next = baseline;
    std::vector<std::string> keys;
    for (const auto& change : input.changes) {
      assertProjectTarget(next, change.target);
      if (!canTarget(change.target, "write")) {
        throw std::runtime_error(
            "Atomic patch target authoring is unavailable");
      }
      const std::string path = normalizeCrowdyStudioPath(change.path);
      const std::string key = crowdyStudioFileKey(change.target, path);
      if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
        throw std::invalid_argument("Atomic patch repeats a target/path");
      }
      keys.push_back(key);
      if (change.content.size() > 65'536) {
        throw std::invalid_argument(
            "Atomic patch file exceeds the 65536-byte limit");
      }
      auto file = findFile(next, change.target, path);
      if (change.operation == CrowdyStudioPatchOperation::Create) {
        if (change.expectedContentHash != "ABSENT" ||
            file != next.files.end()) {
          throw CrowdyStudioRevisionConflictError(
              "Atomic CREATE expected the file to be absent", baseline);
        }
        CrowdyStudioProjectFile created;
        created.target = change.target;
        created.path = path;
        created.content = change.content;
        next.files.push_back(std::move(created));
      } else {
        if (file == next.files.end()) {
          throw CrowdyStudioRevisionConflictError(
              "Atomic REPLACE expected an existing file", baseline);
        }
        if (change.expectedContentHash != sha256(file->content)) {
          throw CrowdyStudioRevisionConflictError(
              "Atomic REPLACE content hash changed", baseline);
        }
        file->content = change.content;
      }
    }
    for (const CrowdyStudioTarget target :
         {CrowdyStudioTarget::Server, CrowdyStudioTarget::Client}) {
      std::size_t count = 0;
      std::size_t bytes = 0;
      for (const auto& file : next.files) {
        if (file.target != target) continue;
        ++count;
        bytes += file.content.size();
      }
      if (count > 8) {
        throw std::invalid_argument(
            "Atomic patch exceeds the 8-file target limit");
      }
      if (bytes > 256 * 1024) {
        throw std::invalid_argument(
            "Atomic patch exceeds the 256-KiB target limit");
      }
    }
  }

  std::string projectContentHash(
      const CrowdyStudioProject& project) const {
    graphql::JVal metadata;
    metadata["name"] = project.metadata.name;
    if (project.metadata.description) {
      metadata["description"] = *project.metadata.description;
    }
    if (project.metadata.serverModuleName) {
      metadata["serverModuleName"] =
          *project.metadata.serverModuleName;
    }
    if (project.metadata.clientModuleName) {
      metadata["clientModuleName"] =
          *project.metadata.clientModuleName;
    }
    metadata["pairingPreference"] =
        toString(project.metadata.pairingPreference);
    std::vector<CrowdyStudioProjectFile> files = project.files;
    sortFiles(files);
    graphql::JArray mappedFiles;
    mappedFiles.reserve(files.size());
    for (const auto& file : files) {
      graphql::JVal mapped;
      mapped["target"] = toString(file.target);
      mapped["path"] = normalizeCrowdyStudioPath(file.path);
      mapped["contentHash"] = sha256(file.content);
      mappedFiles.push_back(std::move(mapped));
    }
    graphql::JVal canonical;
    canonical["contract"] = "crowdy.studio-project-content/1";
    canonical["projectId"] = project.projectId;
    canonical["metadata"] = std::move(metadata);
    canonical["files"] = graphql::JVal(std::move(mappedFiles));
    return sha256(canonical.dump());
  }

  std::string sha256(std::string_view value) const {
    std::array<std::uint8_t, 32> digest{};
    if (!crypto_.sha256(asBytes(value), digest.data())) {
      throw std::runtime_error("Crowdy Studio SHA-256 provider failed");
    }
    std::ostringstream output;
    output << "sha256:" << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest) {
      output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
  }

  void markEdited() {
    for (const auto& [id, listener] : humanEditListeners_) {
      (void)id;
      listener();
    }
    ++editGeneration_;
    state_.saveState = CrowdyStudioSaveState::Saving;
    state_.saveMessage.clear();
    if (state_.agentActivity == CrowdyStudioAgentActivity::Working) {
      state_.agentActivity = CrowdyStudioAgentActivity::Paused;
    }
    if (state_.runtimeSync.state ==
        CrowdyStudioRuntimeSyncState::RunningSaved) {
      state_.runtimeSync.state =
          CrowdyStudioRuntimeSyncState::RunningStale;
    }
    autosaveDueMs_ =
        clock_.monotonicMillis() + options_.autosaveMs;
    notify();
  }

  void reconcileRuntimeSync(std::string_view savedRevisionId) {
    if (state_.runtimeSync.state ==
            CrowdyStudioRuntimeSyncState::RunningSaved ||
        state_.runtimeSync.state ==
            CrowdyStudioRuntimeSyncState::RunningStale) {
      state_.runtimeSync.state =
          state_.runtimeSync.runningRevisionId &&
                  *state_.runtimeSync.runningRevisionId == savedRevisionId
              ? CrowdyStudioRuntimeSyncState::RunningSaved
              : CrowdyStudioRuntimeSyncState::RunningStale;
    }
  }

  void recordBuild(CrowdyStudioTarget target, std::string_view log) {
    if (!state_.buildOutput.empty()) state_.buildOutput += "\n\n";
    state_.buildOutput +=
        "## " + std::string(toString(target)) + "\n";
    state_.buildOutput +=
        log.empty() ? "Compiled successfully." : std::string(log);
    notify();
  }

  void scheduleRetry() {
    retryDueMs_ = clock_.monotonicMillis() + options_.retryMs;
  }

  void sleep(std::int64_t milliseconds) const {
    if (options_.sleep) {
      options_.sleep(milliseconds);
      return;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(milliseconds));
  }

  void checkOperation(std::uint64_t generation) const {
    if (destroyed_ || generation != operationGeneration_) {
      throw OperationCancelledError();
    }
  }

  void checkAgentOperation(
      std::optional<std::uint64_t> generation) const {
    if (generation &&
        (destroyed_ || *generation != agentOperationGeneration_)) {
      throw OperationCancelledError();
    }
  }

  CrowdyStudioDeployResult failedDeployment(
      CrowdyStudioDeployment deployment,
      const CrowdyStudioDeploymentPlan& plan,
      std::string message) {
    state_.runtime = {
        CrowdyStudioPhase::Error, std::nullopt, message};
    notify();
    return {deployment, CrowdyStudioDeployResult::Status::Failed,
            state_.project ? state_.project->revision.id
                           : plan.expectedRevisionId,
            plan.targets, std::move(message)};
  }

  CrowdyStudioDeployResult compileFailedDeployment(
      CrowdyStudioDeployment deployment,
      const CrowdyStudioProject& project,
      const std::vector<CrowdyStudioTarget>& targets) const {
    return {
        deployment, CrowdyStudioDeployResult::Status::CompileFailed,
        project.revision.id, targets,
        state_.runtime.message.empty() ? "Compilation failed"
                                       : state_.runtime.message};
  }

  void validateProjectScope(const CrowdyStudioProject& project) const {
    if (project.appId != options_.appId) {
      throw std::runtime_error(
          "Crowdy Studio project crossed the app scope");
    }
    if (project.gridId && *project.gridId != options_.gridId) {
      throw std::runtime_error(
          "Crowdy Studio project crossed the selected grid scope");
    }
  }

  void assertProjectTarget(const CrowdyStudioProject& project,
                           CrowdyStudioTarget target) const {
    if (!containsTarget(projectTargets(project.kind), target)) {
      throw std::invalid_argument(
          "Crowdy Studio project does not contain the requested target");
    }
  }

  void assertTargetWritable(CrowdyStudioTarget target) const {
    if (!canTarget(target, "write")) {
      throw std::runtime_error(
          "Crowdy Studio target authoring is unavailable on this grid");
    }
  }

  static void validatePairing(
      CrowdyStudioProjectKind kind,
      CrowdyStudioPairingPreference preference) {
    if (kind == CrowdyStudioProjectKind::FullStack &&
        preference == CrowdyStudioPairingPreference::None) {
      throw std::invalid_argument(
          "Full-stack projects require OPTIONAL or REQUIRED pairing");
    }
    if (kind != CrowdyStudioProjectKind::FullStack &&
        preference != CrowdyStudioPairingPreference::None) {
      throw std::invalid_argument(
          "Single-target projects require NONE pairing");
    }
  }

  static void applyPatchField(
      std::optional<std::string>& destination,
      const CrowdyStudioPatchField<std::string>& patch) {
    if (patch.isNull()) destination.reset();
    else if (patch.hasValue()) destination = patch.get();
  }

  static bool containsTarget(
      const std::vector<CrowdyStudioTarget>& targets,
      CrowdyStudioTarget target) {
    return std::find(targets.begin(), targets.end(), target) !=
           targets.end();
  }

  static std::string moduleName(const CrowdyStudioProject& project,
                                CrowdyStudioTarget target) {
    const auto& selected =
        target == CrowdyStudioTarget::Server
            ? project.metadata.serverModuleName
            : project.metadata.clientModuleName;
    if (!selected || selected->empty()) {
      throw std::runtime_error(
          "Crowdy Studio target module name is required");
    }
    return *selected;
  }

  static CrowdyStudioFileRef projectFileRef(
      CrowdyStudioTarget target, std::string_view path) {
    return {CrowdyStudioFileRef::Source::Project, target,
            normalizeCrowdyStudioPath(path), std::nullopt};
  }

  static bool sameFileRef(const CrowdyStudioFileRef& left,
                          const CrowdyStudioFileRef& right) {
    return left.source == right.source && left.target == right.target &&
           left.path == right.path &&
           left.referenceId == right.referenceId;
  }

  static void replaceFileRef(CrowdyStudioFileRef& reference,
                             CrowdyStudioTarget target,
                             std::string_view path,
                             std::string_view replacement) {
    if (reference.source == CrowdyStudioFileRef::Source::Project &&
        reference.target && *reference.target == target &&
        reference.path == path) {
      reference.path = replacement;
    }
  }

  static MutableFileIterator findFile(
      CrowdyStudioProject& project, CrowdyStudioTarget target,
      std::string_view path) {
    const std::string normalized = normalizeCrowdyStudioPath(path);
    return std::find_if(
        project.files.begin(), project.files.end(),
        [&](const CrowdyStudioProjectFile& file) {
          return file.target == target && file.path == normalized;
        });
  }

  static ConstFileIterator findFile(
      const CrowdyStudioProject& project, CrowdyStudioTarget target,
      std::string_view path) {
    const std::string normalized = normalizeCrowdyStudioPath(path);
    return std::find_if(
        project.files.begin(), project.files.end(),
        [&](const CrowdyStudioProjectFile& file) {
          return file.target == target && file.path == normalized;
        });
  }

  static void sortFiles(
      std::vector<CrowdyStudioProjectFile>& files) {
    std::sort(files.begin(), files.end(),
              [](const CrowdyStudioProjectFile& left,
                 const CrowdyStudioProjectFile& right) {
                return crowdyStudioFileKey(left.target, left.path) <
                       crowdyStudioFileKey(right.target, right.path);
              });
  }

  bool fileRefExists(const CrowdyStudioFileRef& reference) const {
    try {
      (void)fileContent(reference);
      return true;
    } catch (...) {
      return false;
    }
  }

  void upsertSummary(const CrowdyStudioProject& project) {
    CrowdyStudioProjectSummary summary;
    summary.projectId = project.projectId;
    summary.gridId = project.gridId;
    summary.name = project.metadata.name;
    summary.kind = project.kind;
    summary.revisionId = project.revision.id;
    summary.serverModuleName = project.metadata.serverModuleName;
    summary.clientModuleName = project.metadata.clientModuleName;
    summary.archived = project.archived;
    summary.updatedAt = project.updatedAt;
    state_.projects.erase(
        std::remove_if(
            state_.projects.begin(), state_.projects.end(),
            [&](const CrowdyStudioProjectSummary& current) {
              return current.projectId == summary.projectId;
            }),
        state_.projects.end());
    state_.projects.push_back(std::move(summary));
    std::sort(
        state_.projects.begin(), state_.projects.end(),
        [](const CrowdyStudioProjectSummary& left,
           const CrowdyStudioProjectSummary& right) {
          return left.updatedAt > right.updatedAt;
        });
  }

  static void upsertReference(
      std::vector<CrowdyStudioReferenceFile>& files,
      const CrowdyStudioReferenceFile& next) {
    files.erase(
        std::remove_if(
            files.begin(), files.end(),
            [&](const CrowdyStudioReferenceFile& current) {
              return current.source == next.source &&
                     current.id == next.id;
            }),
        files.end());
    files.insert(files.begin(), next);
  }

  void upsertCheckpoint(
      const CrowdyStudioCheckpointMetadata& checkpoint) {
    state_.checkpoints.erase(
        std::remove_if(
            state_.checkpoints.begin(), state_.checkpoints.end(),
            [&](const CrowdyStudioCheckpointMetadata& current) {
              return current.checkpointId == checkpoint.checkpointId;
            }),
        state_.checkpoints.end());
    state_.checkpoints.insert(state_.checkpoints.begin(), checkpoint);
  }

  static std::string joinFailures(
      const std::vector<std::string>& failures) {
    std::string joined;
    for (const auto& failure : failures) {
      if (!joined.empty()) joined += " | ";
      joined += failure;
    }
    return joined;
  }

  static void trim(std::string& value) {
    const auto first = std::find_if_not(
        value.begin(), value.end(), [](unsigned char character) {
          return std::isspace(character) != 0;
        });
    const auto last = std::find_if_not(
                          value.rbegin(), value.rend(),
                          [](unsigned char character) {
                            return std::isspace(character) != 0;
                          })
                          .base();
    if (first >= last) {
      value.clear();
    } else {
      value = std::string(first, last);
    }
  }

  static std::size_t surfaceIndex(
      CrowdyStudioPolledSurface surface) {
    return static_cast<std::size_t>(surface);
  }

  static std::string_view saveStateString(
      CrowdyStudioSaveState state) {
    switch (state) {
      case CrowdyStudioSaveState::Saving: return "SAVING";
      case CrowdyStudioSaveState::Saved: return "SAVED";
      case CrowdyStudioSaveState::Conflict: return "CONFLICT";
      case CrowdyStudioSaveState::Offline: return "OFFLINE";
    }
    return "";
  }

  static std::string_view runtimeSyncStateString(
      CrowdyStudioRuntimeSyncState state) {
    switch (state) {
      case CrowdyStudioRuntimeSyncState::NeverRun: return "NEVER_RUN";
      case CrowdyStudioRuntimeSyncState::RunningSaved:
        return "RUNNING_SAVED";
      case CrowdyStudioRuntimeSyncState::RunningStale:
        return "RUNNING_STALE";
      case CrowdyStudioRuntimeSyncState::Stopped: return "STOPPED";
    }
    return "";
  }

  static graphql::JVal runtimeSyncInput(
      const CrowdyStudioRuntimeSync& runtimeSync) {
    graphql::JVal value;
    value["state"] = runtimeSyncStateString(runtimeSync.state);
    if (runtimeSync.savedRevisionId) {
      value["savedRevisionId"] = *runtimeSync.savedRevisionId;
    }
    if (runtimeSync.runningRevisionId) {
      value["runningRevisionId"] = *runtimeSync.runningRevisionId;
    }
    if (runtimeSync.deployment) {
      value["deployment"] =
          *runtimeSync.deployment == CrowdyStudioDeployment::Draft
              ? "DRAFT"
              : "LIVE";
    }
    if (runtimeSync.runningProjectContentHash) {
      value["runningProjectContentHash"] =
          *runtimeSync.runningProjectContentHash;
    }
    if (runtimeSync.runningServerModuleName) {
      value["runningServerModuleName"] =
          *runtimeSync.runningServerModuleName;
    }
    if (runtimeSync.runningClientModuleName) {
      value["runningClientModuleName"] =
          *runtimeSync.runningClientModuleName;
    }
    if (runtimeSync.runningPairingPreference) {
      value["runningPairingPreference"] =
          toString(*runtimeSync.runningPairingPreference);
    }
    if (runtimeSync.startedAtEpochMs) {
      value["startedAtEpochMs"] = *runtimeSync.startedAtEpochMs;
    }
    return value;
  }

  CrowdyStudioProject& requireProject() {
    if (!state_.project) {
      throw std::runtime_error("No Crowdy Studio project is open");
    }
    return *state_.project;
  }

  const CrowdyStudioProject& requireProject() const {
    if (!state_.project) {
      throw std::runtime_error("No Crowdy Studio project is open");
    }
    return *state_.project;
  }

  CrowdyStudioProject& requireWritableProject() {
    CrowdyStudioProject& project = requireProject();
    if (project.archived) {
      throw std::runtime_error(
          "Archived Crowdy Studio projects are read-only");
    }
    return project;
  }

  CrowdyStudioProjectScope scope() const {
    return {options_.appId, options_.gridId};
  }

  void notify() {
    for (const auto& [id, listener] : listeners_) {
      (void)id;
      listener(state_);
    }
  }

  void ensureAlive() const {
    if (destroyed_) {
      throw std::runtime_error("CrowdyStudioController is destroyed");
    }
  }

  CrowdyStudioControllerOptions options_;
  ICrowdyStudioProjectProvider& projectProvider_;
  ICrowdyStudioRuntime& runtime_;
  const core::ICrypto& crypto_;
  const core::IClock& clock_;
  ICrowdyStudioSynchronizationProvider* synchronizationProvider_;
  ICrowdyStudioApprovalGate* approvalGate_;
  CrowdyStudioState state_;
  std::map<ListenerId, StateListener> listeners_;
  std::map<ListenerId, HumanEditListener> humanEditListeners_;
  ListenerId nextListenerId_ = 1;
  std::optional<std::int64_t> autosaveDueMs_;
  std::optional<std::int64_t> retryDueMs_;
  std::optional<std::int64_t> monitorDueMs_;
  std::uint64_t editGeneration_ = 0;
  std::uint64_t persistedGeneration_ = 0;
  std::optional<CrowdyStudioProject> conflictRemote_;
  std::uint64_t operationGeneration_ = 0;
  std::uint64_t agentOperationGeneration_ = 0;
  std::array<bool, 3> surfaceVisible_{false, false, false};
  bool pageVisible_ = true;
  bool destroyed_ = false;
};

}  // namespace crowdy::studio
