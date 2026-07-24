#include "crowdy/studio/integration.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "crowdy/agent/errors.hpp"

namespace crowdy::studio {
namespace {

agent::NativeAgentModeV1 nativeMode(agent::AgentMode mode) {
  switch (mode) {
    case agent::AgentMode::Ask: return agent::NativeAgentModeV1::Ask;
    case agent::AgentMode::Build: return agent::NativeAgentModeV1::Build;
    case agent::AgentMode::Play: return agent::NativeAgentModeV1::Play;
  }
  return agent::NativeAgentModeV1::Ask;
}

bool active(const agent::AgentLease& lease) {
  return lease.status == agent::AgentLeaseStatus::Active;
}

}  // namespace

std::unique_ptr<CrowdyStudioIntegration>
CrowdyStudioIntegration::create(
    CrowdyStudioIntegrationOptions options,
    std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
    std::shared_ptr<ICrowdyStudioRuntime> runtime,
    CrowdyStudioAgentRuntimeFactory agentRuntimeFactory) {
  return std::unique_ptr<CrowdyStudioIntegration>(
      new CrowdyStudioIntegration(
          std::move(options), std::move(projectProvider),
          std::move(runtime), std::move(agentRuntimeFactory)));
}

CrowdyStudioIntegration::CrowdyStudioIntegration(
    CrowdyStudioIntegrationOptions options,
    std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
    std::shared_ptr<ICrowdyStudioRuntime> runtime,
    CrowdyStudioAgentRuntimeFactory agentRuntimeFactory)
    : projectProvider_(std::move(projectProvider)),
      clientRuntimeOwner_(std::move(options.clientRuntime)),
      runtime_(std::move(runtime)),
      crypto_(std::move(options.crypto)),
      editorAdapter_(std::move(options.editor)),
      synchronization_(std::move(options.synchronization)),
      approval_(std::move(options.approval)),
      layout_(std::move(options.layout)),
      control_(std::move(options.control)),
      leaseManager_(options.leaseManager),
      platformPoll_(std::move(options.platformPoll)) {
  if (!projectProvider_ || !runtime_ || !crypto_ || !leaseManager_) {
    throw std::invalid_argument(
        "Crowdy Studio integration requires owned project/runtime/crypto "
        "services and an external lease manager");
  }

  controller_ = std::make_unique<CrowdyStudioController>(
      std::move(options.studio), *projectProvider_, *runtime_, *crypto_,
      options.clock ? *options.clock : core::systemClock(),
      synchronization_.get(), approval_.get());
  if (editorAdapter_) {
    editorBridge_ = std::make_unique<CrowdyStudioEditorBridge>(
        *controller_, editorAdapter_);
  }

  auto hostOptions = std::move(options.studioHost);
  const auto fallbackHostSession = hostOptions.sessionId;
  const auto fallbackHostEpoch = hostOptions.clientEpoch;
  const auto fallbackHostContext = hostOptions.contextVersion;
  const auto fallbackHostLeases = hostOptions.leaseKinds;
  const auto fallbackHostCapability =
      hostOptions.hostCapabilityRevision;
  const auto fallbackHostLeaseActive = hostOptions.isLeaseActive;
  const auto fallbackDiagnostics = hostOptions.localDiagnostics;

  hostOptions.sessionId = [this, fallbackHostSession] {
    auto current = currentSessionId();
    if (agentRuntime_) return current;
    return current || !fallbackHostSession ? current
                                           : fallbackHostSession();
  };
  hostOptions.clientEpoch = [this, fallbackHostEpoch] {
    auto current = currentClientEpoch();
    if (agentRuntime_) return current;
    return current || !fallbackHostEpoch ? current
                                         : fallbackHostEpoch();
  };
  hostOptions.contextVersion = [this, fallbackHostContext] {
    if (agentRuntime_) return currentContextVersion();
    if (fallbackHostContext) {
      const std::string fallback = fallbackHostContext();
      if (!fallback.empty()) return fallback;
    }
    return currentContextVersion();
  };
  hostOptions.leaseKinds = [this, fallbackHostLeases] {
    auto values = activeLeaseKinds();
    if (fallbackHostLeases) {
      auto fallback = fallbackHostLeases();
      values.insert(values.end(), fallback.begin(), fallback.end());
      std::sort(values.begin(), values.end());
      values.erase(std::unique(values.begin(), values.end()),
                   values.end());
    }
    return values;
  };
  hostOptions.hostCapabilityRevision =
      [this, fallbackHostCapability] {
        auto revision = hostCapabilityRevision();
        return revision || !fallbackHostCapability
                   ? revision
                   : fallbackHostCapability();
      };
  hostOptions.isLeaseActive =
      [this, fallbackHostLeaseActive](
          std::string_view id, player_host::LeaseKindV1 kind) {
        if (agentRuntime_) {
          return isLeaseActive(id, kind) &&
                 (!fallbackHostLeaseActive ||
                  fallbackHostLeaseActive(id, kind));
        }
        return fallbackHostLeaseActive &&
               fallbackHostLeaseActive(id, kind);
      };
  hostOptions.localDiagnostics =
      [this, fallbackDiagnostics] {
        std::vector<CrowdyStudioEditorDiagnostic> diagnostics;
        if (editorBridge_) diagnostics = editorBridge_->localDiagnostics();
        if (fallbackDiagnostics) {
          auto fallback = fallbackDiagnostics();
          diagnostics.insert(
              diagnostics.end(),
              std::make_move_iterator(fallback.begin()),
              std::make_move_iterator(fallback.end()));
        }
        return diagnostics;
      };

  studioHost_ =
      std::make_unique<CrowdyStudioControllerHostAdapter>(
          *controller_, *crypto_, std::move(hostOptions));

  auto nativeOptions = std::move(options.nativeTools);
  const auto fallbackSession = nativeOptions.session_id;
  const auto fallbackEpoch = nativeOptions.client_epoch;
  const auto fallbackContext = nativeOptions.context_version;
  const auto fallbackMode = nativeOptions.mode;
  const auto fallbackLeaseActive = nativeOptions.is_lease_active;
  nativeOptions.session_id = [this, fallbackSession] {
    auto current = currentSessionId();
    if (agentRuntime_) return current;
    return current || !fallbackSession ? current : fallbackSession();
  };
  nativeOptions.client_epoch = [this, fallbackEpoch] {
    auto current = currentClientEpoch();
    if (agentRuntime_) return current;
    return current || !fallbackEpoch ? current : fallbackEpoch();
  };
  nativeOptions.context_version = [this, fallbackContext] {
    if (agentRuntime_) return currentContextVersion();
    if (fallbackContext) {
      const std::string fallback = fallbackContext();
      if (!fallback.empty()) return fallback;
    }
    return currentContextVersion();
  };
  nativeOptions.mode = [this, fallbackMode] {
    return agentRuntime_ ? currentMode()
                         : fallbackMode ? fallbackMode()
                                        : agent::NativeAgentModeV1::Ask;
  };
  nativeOptions.is_lease_active =
      [this, fallbackLeaseActive](
          std::string_view id, player_host::LeaseKindV1 kind) {
        if (agentRuntime_) {
          return isLeaseActive(id, kind) &&
                 (!fallbackLeaseActive ||
                  fallbackLeaseActive(id, kind));
        }
        return fallbackLeaseActive && fallbackLeaseActive(id, kind);
      };

  nativeDispatcher_ = std::make_unique<agent::NativeToolDispatcherV1>(
      *leaseManager_, studioHost_.get(), std::move(nativeOptions));
  browserDispatcher_ =
      std::make_unique<agent::NativeBrowserToolDispatcherAdapter>(
          *nativeDispatcher_);

  if (options.agent) {
    if (!agentRuntimeFactory) {
      throw std::invalid_argument(
          "Agent controller options require an agent runtime factory");
    }
    auto agentOptions = std::move(*options.agent);
    const auto beforeAgentWork = agentOptions.beforeAgentWork;
    const auto onEpochAttached = agentOptions.onEpochAttached;
    const auto onLeaseChanged = agentOptions.onLeaseChanged;
    const auto onPreempt = agentOptions.onPreempt;
    agentOptions.browserDispatcher = browserDispatcher_.get();
    agentOptions.beforeAgentWork =
        [this, beforeAgentWork](agent::AgentMode mode) {
          if (const auto failure = prepareForAgentWork(mode)) {
            return failure;
          }
          if (beforeAgentWork) {
            if (const auto failure = beforeAgentWork(mode)) {
              controller_->finishAgentWork(true);
              return failure;
            }
          }
          return std::optional<agent::AgentError>{};
        };
    agentOptions.onEpochAttached =
        [this, onEpochAttached](std::string_view epoch) {
          epochAttached(epoch);
          if (onEpochAttached) onEpochAttached(epoch);
        };
    agentOptions.onLeaseChanged =
        [this, onLeaseChanged](const agent::AgentLease& lease) {
          leaseChanged(lease);
          if (onLeaseChanged) onLeaseChanged(lease);
        };
    agentOptions.onPreempt =
        [this, onPreempt](agent::AgentPreemptionReason reason) {
          preempted(reason);
          if (onPreempt) onPreempt(reason);
        };
    agentRuntime_ = agentRuntimeFactory(std::move(agentOptions));
    if (!agentRuntime_) {
      throw std::runtime_error(
          "Crowdy Studio agent runtime factory returned null");
    }
  }

  stateSubscription_ = controller_->subscribe(
      [this](const CrowdyStudioState& state) {
        if (!disposed_) stateChanged(state);
      });
  humanEditSubscription_ = controller_->onHumanEdit([this] {
    if (!disposed_ && agentRuntime_) {
      agentRuntime_->controller().preemptForHumanEdit();
    }
  });

  if (options.autoInitializeStudio || options.autoInitializeAgent) {
    controller_->initialize();
    studioInitialized_ = true;
  }
  if (options.autoInitializeAgent && agentRuntime_) {
    agentRuntime_->controller().initialize();
    agentInitialized_ = true;
  }
}

CrowdyStudioIntegration::~CrowdyStudioIntegration() { dispose(); }

agent::CrowdyStudioAgentController*
CrowdyStudioIntegration::agentController() {
  return agentRuntime_ ? &agentRuntime_->controller() : nullptr;
}

const agent::CrowdyStudioAgentController*
CrowdyStudioIntegration::agentController() const {
  return agentRuntime_ ? &agentRuntime_->controller() : nullptr;
}

void CrowdyStudioIntegration::initialize() {
  if (disposed_) {
    throw std::runtime_error("CrowdyStudioIntegration is disposed");
  }
  if (!studioInitialized_) {
    controller_->initialize();
    studioInitialized_ = true;
  }
  if (agentRuntime_ && !agentInitialized_) {
    agentRuntime_->controller().initialize();
    agentInitialized_ = true;
  }
}

std::size_t CrowdyStudioIntegration::poll() {
  if (disposed_) return 0;
  if (agentRuntime_) return agentRuntime_->poll();
  return platformPoll_ ? platformPoll_() : 0;
}

std::size_t CrowdyStudioIntegration::tick() {
  if (disposed_) return 0;
  const std::size_t callbacks = poll();
  controller_->tick();
  if (!agentRuntime_) nativeDispatcher_->tick();
  if (layout_) layout_->tick();
  if (control_) control_->tick();
  return callbacks;
}

void CrowdyStudioIntegration::setPageVisible(bool visible) {
  if (disposed_) return;
  controller_->setPageVisible(visible);
  if (agentRuntime_) agentRuntime_->controller().setPageVisible(visible);
}

void CrowdyStudioIntegration::relayout() {
  if (disposed_) return;
  if (editorBridge_) editorBridge_->relayout();
  if (layout_) layout_->relayout();
}

void CrowdyStudioIntegration::dispose() noexcept {
  if (disposed_) return;
  disposed_ = true;
  if (stateSubscription_ != 0 && controller_) {
    controller_->unsubscribe(stateSubscription_);
    stateSubscription_ = 0;
  }
  if (humanEditSubscription_ != 0 && controller_) {
    controller_->unsubscribeHumanEdit(humanEditSubscription_);
    humanEditSubscription_ = 0;
  }
  if (control_) control_->dispose();
  if (layout_) layout_->dispose();
  if (agentRuntime_) {
    agentRuntime_->controller().destroy();
    agentRuntime_.reset();
  }
  browserDispatcher_.reset();
  if (nativeDispatcher_) {
    nativeDispatcher_->cancelActive(
        player_host::PreemptionReasonV1::SESSION_CLOSED);
    nativeDispatcher_.reset();
  }
  if (studioHost_) {
    studioHost_->clearAgentOperation(
        player_host::PreemptionReasonV1::SESSION_CLOSED);
    studioHost_.reset();
  }
  if (editorBridge_) {
    editorBridge_->dispose();
    editorBridge_.reset();
  }
  if (controller_) {
    controller_->destroy();
    controller_.reset();
  }
}

std::optional<std::string>
CrowdyStudioIntegration::currentSessionId() const {
  if (!agentRuntime_) return std::nullopt;
  const auto& session = agentRuntime_->controller().state().session;
  return session ? std::optional<std::string>(session->sessionId)
                 : std::nullopt;
}

std::optional<std::string>
CrowdyStudioIntegration::currentClientEpoch() const {
  return agentRuntime_ ? agentRuntime_->controller().state().clientEpoch
                       : std::nullopt;
}

std::string CrowdyStudioIntegration::currentContextVersion() const {
  if (agentRuntime_) {
    const auto& session = agentRuntime_->controller().state().session;
    if (session && !session->contextVersion.empty()) {
      return session->contextVersion;
    }
  }
  return controller_->getAgentContext().contextVersion;
}

agent::NativeAgentModeV1 CrowdyStudioIntegration::currentMode() const {
  if (!agentRuntime_) return agent::NativeAgentModeV1::Ask;
  const auto& session = agentRuntime_->controller().state().session;
  return session ? nativeMode(session->mode)
                 : agent::NativeAgentModeV1::Ask;
}

bool CrowdyStudioIntegration::isLeaseActive(
    std::string_view leaseId, player_host::LeaseKindV1 kind) const {
  if (!agentRuntime_) return false;
  const auto expected = kind == player_host::LeaseKindV1::Workspace
                            ? agent::AgentLeaseKind::Workspace
                            : agent::AgentLeaseKind::Play;
  const auto& leases = agentRuntime_->controller().state().leases;
  return std::any_of(
      leases.begin(), leases.end(), [&](const agent::AgentLease& lease) {
        return lease.leaseId == leaseId && lease.kind == expected &&
               active(lease);
      });
}

std::vector<player_host::LeaseKindV1>
CrowdyStudioIntegration::activeLeaseKinds() const {
  std::vector<player_host::LeaseKindV1> values;
  if (!agentRuntime_) return values;
  for (const auto& lease : agentRuntime_->controller().state().leases) {
    if (!active(lease)) continue;
    values.push_back(
        lease.kind == agent::AgentLeaseKind::Workspace
            ? player_host::LeaseKindV1::Workspace
            : player_host::LeaseKindV1::Play);
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::optional<std::string>
CrowdyStudioIntegration::hostCapabilityRevision() const {
  if (!leaseManager_) return std::nullopt;
  const auto snapshot = leaseManager_->snapshot();
  return snapshot.capabilities
             ? std::optional<std::string>(
                   snapshot.capabilities->revision)
             : std::nullopt;
}

std::optional<agent::AgentError>
CrowdyStudioIntegration::prepareForAgentWork(agent::AgentMode) {
  try {
    (void)controller_->prepareForAgentWork();
    return std::nullopt;
  } catch (const CrowdyStudioRevisionConflictError& failure) {
    return agent::makeAgentError(
        "AGENT_CONTEXT_STALE", failure.what());
  } catch (const std::exception& failure) {
    return agent::makeAgentError(
        "AGENT_TOOL_FAILED", failure.what());
  } catch (...) {
    return agent::makeAgentError(
        "AGENT_TOOL_FAILED",
        "Crowdy Studio could not prepare for agent work");
  }
}

void CrowdyStudioIntegration::epochAttached(std::string_view epoch) {
  if (const auto failure = leaseManager_->attach(std::string(epoch))) {
    leaseManager_->preempt(
        player_host::PreemptionReasonV1::CLIENT_REATTACHED);
  }
  if (control_) control_->onEpochAttached(epoch, *leaseManager_);
}

void CrowdyStudioIntegration::leaseChanged(
    const agent::AgentLease& lease) {
  if (control_) control_->onLeaseChanged(lease, *leaseManager_);
}

void CrowdyStudioIntegration::preempted(
    agent::AgentPreemptionReason reason) {
  if (control_) control_->onPreempt(reason, *leaseManager_);
}

void CrowdyStudioIntegration::stateChanged(
    const CrowdyStudioState& state) {
  const std::optional<std::string> next =
      state.project
          ? std::optional<std::string>(state.project->projectId)
          : std::nullopt;
  if (next == selectedProjectId_) return;
  selectedProjectId_ = next;
  if (agentRuntime_) {
    agentRuntime_->controller().projectSelectionChanged(next);
  }
}

}  // namespace crowdy::studio
