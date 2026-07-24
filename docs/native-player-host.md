# Native player-host integration

`PlayerHostAdapterV1` is the only boundary that executes local game intent.
Implement it over the same movement, inventory, interaction, crafting, combat,
chat, and travel services used by human controls. Do not give it a
`CrowdyClient`, transport, socket, filesystem, shell, provider, or generic
tool executor.

```cpp
class MyPlayerHost final : public crowdy::player_host::PlayerHostAdapterV1 {
 public:
  void capabilities(crowdy::player_host::CancellationTokenV1 cancel,
                    crowdy::player_host::CapabilitiesCallbackV1 done) override;
  void observe(const crowdy::player_host::ObserveRequestV1& request,
               crowdy::player_host::CancellationTokenV1 cancel,
               crowdy::player_host::ObservationCallbackV1 done) override;
  void dispatch(const crowdy::player_host::GameCommandV1& command,
                const crowdy::player_host::ValidatedGateV1& gate,
                crowdy::player_host::CancellationTokenV1 cancel,
                crowdy::player_host::CommandCallbackV1 done) override;
  void clearAgentIntent(
      crowdy::player_host::PreemptionReasonV1 reason) noexcept override;
};
```

Wire the authority gate, typed native router, and controller adapter:

```cpp
MyPlayerHost host;
crowdy::player_host::AgentControlLeaseManager leases(
    host,
    {.context_version = [&] { return currentGameContextVersion(); }});

crowdy::agent::NativeToolDispatcherV1 nativeTools(
    leases, studioHost,
    {.session_id = [&] { return attachedSessionId(); },
     .client_epoch = [&] { return attachedClientEpoch(); },
     .context_version = [&] { return currentGameContextVersion(); },
     .mode = [&] { return currentNativeAgentMode(); },
     .is_lease_active = [&](std::string_view id, auto kind) {
       return visibleLeaseIsActive(id, kind);
     },
     .validate_argument_hash = validateCanonicalArgumentHash,
     .validate_approval_grant = validateExactApprovalGrant});

crowdy::agent::NativeBrowserToolDispatcherAdapter localTools(
    nativeTools);

crowdy::agent::CrowdyStudioAgentControllerOptions options;
options.sessionId = savedSessionId;
options.browserDispatcher = &localTools;
options.onEpochAttached = [&](std::string_view epoch) {
  leases.attach(std::string(epoch));
};
options.onLeaseChanged = [&](const crowdy::agent::AgentLease& lease) {
  applyVisiblePlayLease(leases, lease); // exact field/scope conversion
};

auto agent = client.createCrowdyStudioAgentController(std::move(options));
agent->controller().initialize();

while (running) {
  agent->poll(); // WS/HTTP delivery, controller timers, native tool deadlines
}
```

Human input must synchronously preempt local intent before any asynchronous
cleanup:

```cpp
leases.onHumanInput();
localTools.cancelActive(
    crowdy::agent::AgentPreemptionReason::HumanInput);
```

`NativeBrowserToolDispatcherAdapter` validates canonical input schemas,
converts JSON field-by-field into closed C++ variants, propagates
cancellation, timing, context, output, and typed errors, and converts output
back to canonical validated JSON. Unknown/server/provider tools fail closed.
Call `clearClosedSession()` only after the attached session is closed or
fenced; otherwise an effect could execute twice.
