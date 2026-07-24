# Native Agentic Studio API

CrowdyCPP exposes the portable `crowdy.studio-agent/1` runtime without a DOM,
provider client, unrestricted SDK bridge, or raw network/tool authority.

## Layers

- `client.crowdyStudioAgent()` is the exact generated-document GraphQL surface.
  Session, event, descriptor, budget, lease, approval, and run operations use
  the Game API. App policy, sanitized usage, and operator policy/kill
  operations use the Management API.
- `crowdy::agent::CrowdyStudioAgentGraphQLTransport` maps those documents onto
  the typed `IAgentTransport` contract.
- `crowdy::agent::CrowdyStudioAgentController` owns attach epochs, durable
  replay, gap filling, acknowledgements, reducer state, policy/registry
  repinning, approvals, leases, budgets, heartbeat renewal, reconnect fencing,
  and late-result rejection.
- `canonicalAgentToolRegistryV1()` is the immutable canonical 28-tool Game API
  registry. Descriptor and registry SHA-256 values are checked against the
  installed CrowdyJS v12 fixture before use.

## Minimal native loop

```cpp
auto transport =
    crowdy::agent::CrowdyStudioAgentGraphQLTransport(
        client.crowdyStudioAgent());

crowdy::agent::CrowdyStudioAgentControllerOptions options;
options.transport = &transport;
options.dispatcher = client.crowdyStudioAgent().dispatcher();
options.sessionId = savedSessionId;
options.onStateChange = [](const auto& state) {
  // Copy state into engine/UI models on the poll thread.
};

crowdy::agent::CrowdyStudioAgentController agent(std::move(options));
agent.initialize();

while (running) {
  client.poll();  // HTTP completions
  agent.poll();   // subscriptions/fallback replay, timers, reducer callbacks
}
```

Callbacks from HTTP, subscriptions, and host tool execution are fenced and
land through a `graphql::Dispatcher`; no engine object is touched on a network
thread. The controller has no internal worker thread.

## Realtime adapter contract

The GraphQL-WS integration implements `IAgentEventSubscriptionAdapter`.
`subscribe()` receives only:

- `sessionId`
- the exclusive `afterSeq` durable cursor
- the exact attached `clientEpoch`

It emits typed `AgentEvent` values and returns a closeable handle. Delivery may
be at least once; the controller deduplicates, orders, fills gaps through
`history`, and acknowledges only contiguous sequences. Adapter callbacks may
arrive on any thread because the controller posts them to its dispatcher.

When no realtime adapter is supplied, the controller owns a
`PollingAgentEventSubscriptionAdapter`. It replays the same durable history
from `poll()` and is suitable for deterministic tests or platforms awaiting a
WebSocket implementation.

## Studio and player-host seams

Native Studio and game hosts implement `IAgentBrowserToolDispatcher`:

- `dispatch(invocation, callback)` executes one exact descriptor-pinned
  `BROWSER` invocation through an allowlisted host service.
- `cancelActive(reason)` aborts pending host work synchronously on human or
  context preemption.
- `clearClosedSession()` may release execute-once records only after the
  session is closed/fenced.

The host implementation must revalidate descriptor digest, epoch, context,
lease, approval, input/output schema, deadline, and execute-once identity.
Ambiguous effects return `OUTCOME_UNKNOWN` and are never retried. Studio
project edits and player commands must route through the same intent services
as human actions. They must not receive a raw `CrowdyClient`, GraphQL executor,
UDP connection, provider credential, shell, filesystem, or generic tool
callback.

Studio should also provide:

- `beforeAgentWork(mode)` to flush autosave and fail a human turn on unresolved
  revision conflicts;
- `onPreempt(reason)` to stop local effects before best-effort server cleanup;
- `onLeaseChanged(lease)` to bind/release visible workspace or Play authority.

This phase defines these interfaces only. Project/editor effects and native
player-host command execution belong in their host integrations.

## Safety controls

`pause`, `resume`, `cancelRun`, `stop`, `revokeLease`, and `close` are explicit
controller operations. `stop()` first preempts locally, then best-effort
revokes active leases, cancels the current run, and pauses the session.
Reattach allocates a newer epoch and drops callbacks/results from every older
subscription or tool dispatch.
