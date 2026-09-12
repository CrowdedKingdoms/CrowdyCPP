# Native player-host integration

`crowdy/player_host/` is the typed **observation** contract
(`crowdy.player-host/1`): what a game can say about the controlled player and
their surroundings, in a shape a model or a tool can read. Since 0.34.0 it is
observation only. The lease manager, control gate and native tool dispatchers
that used to sit on top of it executed commands for the Crowdy Agent
orchestrator; that orchestrator is retired, the agent runs in the browser and
only observes (`game_observe`), and a native engine has no agent pane at all.
Nothing in this SDK dispatches a `GameCommandV1` any more.

Implement the adapter over the same services your human controls read from.
Do not give it a `CrowdyClient`, transport, socket, filesystem, shell,
provider, or generic tool executor.

```cpp
class MyPlayerHost final : public crowdy::player_host::PlayerHostAdapterV1 {
 public:
  void capabilities(crowdy::player_host::CancellationTokenV1 cancel,
                    crowdy::player_host::CapabilitiesCallbackV1 done) override;
  void observe(const crowdy::player_host::ObserveRequestV1& request,
               crowdy::player_host::CancellationTokenV1 cancel,
               crowdy::player_host::ObservationCallbackV1 done) override;
  // Kept on the interface for source compatibility; nothing calls them.
  void dispatch(const crowdy::player_host::GameCommandV1& command,
                const crowdy::player_host::ValidatedGateV1& gate,
                crowdy::player_host::CancellationTokenV1 cancel,
                crowdy::player_host::CommandCallbackV1 done) override;
  void clearAgentIntent(
      crowdy::player_host::PreemptionReasonV1 reason) noexcept override;
};
```

What still matters:

- `observe()` answers with a `GameObservationV1` that passes
  `validateGameObservationV1`: decimal-string coordinates, a bounded list of
  nearby actors (the request's `max_nearby_actors` / `max_nearby_voxels` are
  hard limits), an `observed_at` / `expires_at` window.
- `capabilities()` still describes the controlled entity and the observation
  bounds; the `commands` list is informational.
- The schemas (`crowdy/player_host/schemas.hpp`) and the closed
  `PreemptionReasonV1` vocabulary (`kClosedPreemptionReasonsV1`,
  `preemptionReasonName`, `preemptionReasonFromName`) are unchanged in
  content. The vocabulary used to be the API's enum; the contract carries it
  itself now, in the same order CrowdyJS's `player-host/agent-types` does.

`CrowdyStudioIntegration` accepts a `playerHost` pointer and hands it back from
`playerHost()`; it does not call it. The pointer must outlive the integration.
See [Native Studio integration](native-studio-integration.md).

World coordinates, distances, health values, fuel, revisions and other contract
values that may exceed a native or JSON number remain decimal strings. The
typed schemas reject non-canonical forms before an adapter runs.
