#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/agent/native_tool_dispatcher.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/studio/controller.hpp"
#include "crowdy/studio/editor.hpp"

namespace crowdy::studio {

struct CrowdyStudioControllerHostAdapterOptions {
  std::function<std::optional<std::string>()> sessionId;
  std::function<std::optional<std::string>()> clientEpoch;
  std::function<std::string()> contextVersion;
  std::function<std::vector<player_host::LeaseKindV1>()> leaseKinds;
  std::function<std::optional<std::string>()> hostCapabilityRevision;
  std::function<bool(std::string_view, player_host::LeaseKindV1)>
      isLeaseActive;

  /**
   * Revalidates a grant against the exact typed request at the final host
   * boundary. LIVE invoke fails closed when this validator is absent.
   * LIVE deploy is additionally checked by ICrowdyStudioApprovalGate.
   */
  std::function<bool(
      agent::StudioNativeToolKindV1,
      const agent::StudioNativeToolRequestV1&,
      const agent::ValidatedStudioGateV1&)>
      validateApprovalGrant;

  std::function<std::vector<CrowdyStudioEditorDiagnostic>()>
      localDiagnostics;
};

/**
 * Concrete implementation of the 11 native Studio tools over
 * CrowdyStudioController. The adapter owns no transport or generic client and
 * cannot execute an unregistered operation.
 */
class CrowdyStudioControllerHostAdapter final
    : public agent::CrowdyStudioHostAdapter {
 public:
  CrowdyStudioControllerHostAdapter(
      CrowdyStudioController& controller, const core::ICrypto& crypto,
      CrowdyStudioControllerHostAdapterOptions options = {});
  ~CrowdyStudioControllerHostAdapter() override;

  CrowdyStudioControllerHostAdapter(
      const CrowdyStudioControllerHostAdapter&) = delete;
  CrowdyStudioControllerHostAdapter& operator=(
      const CrowdyStudioControllerHostAdapter&) = delete;

  void dispatch(
      agent::StudioNativeToolKindV1 kind,
      const agent::StudioNativeToolRequestV1& request,
      const agent::ValidatedStudioGateV1& gate,
      player_host::CancellationTokenV1 cancellation,
      agent::StudioToolCallbackV1 callback) override;

  void clearAgentOperation(
      player_host::PreemptionReasonV1 reason) noexcept override;

 private:
  using StudioResult = player_host::AdapterResultV1<
      agent::StudioNativeToolOutputV1>;

  std::optional<player_host::AgentErrorV1> validateGate(
      agent::StudioNativeToolKindV1 kind,
      const agent::StudioNativeToolRequestV1& request,
      const agent::ValidatedStudioGateV1& gate,
      const player_host::CancellationTokenV1& cancellation) const;
  agent::StudioNativeToolOutputV1 execute(
      agent::StudioNativeToolKindV1 kind,
      const agent::StudioNativeToolRequestV1& request,
      const agent::ValidatedStudioGateV1& gate,
      const player_host::CancellationTokenV1& cancellation,
      bool& effectStarted);

  agent::StudioContextV1 contextProjection() const;
  agent::StudioStateV1 stateProjection() const;
  agent::StudioRuntimeStatusV1 runtimeProjection() const;
  agent::StudioDiagnosticsV1 diagnosticsProjection() const;

  std::string sha256(std::string_view value) const;
  bool gateStillCurrent(
      const agent::ValidatedStudioGateV1& gate) const;

  CrowdyStudioController& controller_;
  const core::ICrypto& crypto_;
  CrowdyStudioControllerHostAdapterOptions options_;
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<bool> disposed_{false};
};

}  // namespace crowdy::studio
