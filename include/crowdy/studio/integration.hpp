#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "crowdy/agent/client_runtime.hpp"
#include "crowdy/agent/native_browser_dispatcher.hpp"
#include "crowdy/player_host/lease_manager.hpp"
#include "crowdy/studio/editor.hpp"
#include "crowdy/studio/host_adapter.hpp"

namespace crowdy::studio {

/**
 * Forward-compatible layout ownership seam. The parallel layout phase can
 * provide a concrete implementation without CrowdyStudioIntegration
 * duplicating persistence or pane semantics.
 */
class ICrowdyStudioIntegrationLayout {
 public:
  virtual ~ICrowdyStudioIntegrationLayout() = default;
  virtual void relayout() = 0;
  virtual void tick() {}
  virtual void dispose() noexcept = 0;
};

/**
 * Forward-compatible human-control seam. The parallel control-gate phase can
 * consume epoch, lease, and preemption transitions while this assembly keeps
 * ownership and callback ordering stable.
 */
class ICrowdyStudioIntegrationControl {
 public:
  virtual ~ICrowdyStudioIntegrationControl() = default;
  virtual void onEpochAttached(
      std::string_view,
      player_host::AgentControlLeaseManager&) {}
  virtual void onLeaseChanged(
      const agent::AgentLease&,
      player_host::AgentControlLeaseManager&) {}
  virtual void onPreempt(
      agent::AgentPreemptionReason,
      player_host::AgentControlLeaseManager&) {}
  virtual void tick() {}
  virtual void dispose() noexcept = 0;
};

using CrowdyStudioAgentRuntimeFactory = std::function<
    std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime>(
        agent::CrowdyStudioAgentControllerOptions)>;

struct CrowdyStudioIntegrationOptions {
  CrowdyStudioControllerOptions studio;
  std::shared_ptr<const core::ICrypto> crypto;
  const core::IClock* clock = nullptr;
  std::shared_ptr<ICrowdyStudioEditorAdapter> editor;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntime;
  std::shared_ptr<ICrowdyStudioSynchronizationProvider> synchronization;
  std::shared_ptr<ICrowdyStudioApprovalGate> approval;

  /** Externally owned and required; it must outlive the integration. */
  player_host::AgentControlLeaseManager* leaseManager = nullptr;
  agent::NativeToolDispatcherOptionsV1 nativeTools;
  CrowdyStudioControllerHostAdapterOptions studioHost;
  std::optional<agent::CrowdyStudioAgentControllerOptions> agent;

  std::shared_ptr<ICrowdyStudioIntegrationLayout> layout;
  std::shared_ptr<ICrowdyStudioIntegrationControl> control;
  std::function<std::size_t()> platformPoll;
  bool autoInitializeStudio = false;
  bool autoInitializeAgent = false;
};

/**
 * Destruction-safe native Studio assembly. Shared providers and runtimes are
 * retained behind the controller; agent/browser/native adapters are destroyed
 * before the controller; externally supplied lease authority is never owned.
 */
class CrowdyStudioIntegration {
 public:
  static std::unique_ptr<CrowdyStudioIntegration> create(
      CrowdyStudioIntegrationOptions options,
      std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
      std::shared_ptr<ICrowdyStudioRuntime> runtime,
      CrowdyStudioAgentRuntimeFactory agentRuntimeFactory = {});

  ~CrowdyStudioIntegration();
  CrowdyStudioIntegration(const CrowdyStudioIntegration&) = delete;
  CrowdyStudioIntegration& operator=(const CrowdyStudioIntegration&) =
      delete;

  CrowdyStudioController& studio() { return *controller_; }
  const CrowdyStudioController& studio() const { return *controller_; }
  CrowdyStudioEditorBridge* editor() { return editorBridge_.get(); }
  const CrowdyStudioEditorBridge* editor() const {
    return editorBridge_.get();
  }
  agent::CrowdyStudioAgentController* agentController();
  const agent::CrowdyStudioAgentController* agentController() const;
  agent::NativeToolDispatcherV1& nativeTools() {
    return *nativeDispatcher_;
  }

  void initialize();
  std::size_t poll();
  std::size_t tick();
  void setPageVisible(bool visible);
  void relayout();
  void dispose() noexcept;
  bool disposed() const noexcept { return disposed_; }

 private:
  CrowdyStudioIntegration(
      CrowdyStudioIntegrationOptions options,
      std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider,
      std::shared_ptr<ICrowdyStudioRuntime> runtime,
      CrowdyStudioAgentRuntimeFactory agentRuntimeFactory);

  std::optional<std::string> currentSessionId() const;
  std::optional<std::string> currentClientEpoch() const;
  std::string currentContextVersion() const;
  agent::NativeAgentModeV1 currentMode() const;
  bool isLeaseActive(
      std::string_view leaseId,
      player_host::LeaseKindV1 kind) const;
  std::vector<player_host::LeaseKindV1> activeLeaseKinds() const;
  std::optional<std::string> hostCapabilityRevision() const;
  std::optional<agent::AgentError> prepareForAgentWork(
      agent::AgentMode mode);
  void epochAttached(std::string_view epoch);
  void leaseChanged(const agent::AgentLease& lease);
  void preempted(agent::AgentPreemptionReason reason);
  void stateChanged(const CrowdyStudioState& state);

  // Owners first: reverse member destruction tears down the dependency graph.
  std::shared_ptr<ICrowdyStudioProjectProvider> projectProvider_;
  std::shared_ptr<ICrowdyStudioClientRuntime> clientRuntimeOwner_;
  std::shared_ptr<ICrowdyStudioRuntime> runtime_;
  std::shared_ptr<const core::ICrypto> crypto_;
  std::shared_ptr<ICrowdyStudioEditorAdapter> editorAdapter_;
  std::shared_ptr<ICrowdyStudioSynchronizationProvider> synchronization_;
  std::shared_ptr<ICrowdyStudioApprovalGate> approval_;
  std::shared_ptr<ICrowdyStudioIntegrationLayout> layout_;
  std::shared_ptr<ICrowdyStudioIntegrationControl> control_;
  player_host::AgentControlLeaseManager* leaseManager_ = nullptr;
  std::function<std::size_t()> platformPoll_;

  std::unique_ptr<CrowdyStudioController> controller_;
  std::unique_ptr<CrowdyStudioEditorBridge> editorBridge_;
  std::unique_ptr<CrowdyStudioControllerHostAdapter> studioHost_;
  std::unique_ptr<agent::NativeToolDispatcherV1> nativeDispatcher_;
  std::unique_ptr<agent::NativeBrowserToolDispatcherAdapter>
      browserDispatcher_;
  std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime> agentRuntime_;

  CrowdyStudioController::ListenerId stateSubscription_ = 0;
  CrowdyStudioController::ListenerId humanEditSubscription_ = 0;
  std::optional<std::string> selectedProjectId_;
  bool studioInitialized_ = false;
  bool agentInitialized_ = false;
  bool disposed_ = false;
};

}  // namespace crowdy::studio
