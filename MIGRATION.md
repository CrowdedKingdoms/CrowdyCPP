# CrowdyCPP migration notes

## 0.14.0 strict parity

0.14.0 is additive. It closes every portable CrowdyJS v12 parity gap and adds
typed wrappers for current platform SDL additions that postdate the pinned JS
release.

- Prefer `client.createCrowdyStudioAgentController(options)` for production
  agent construction. It owns `CrowdyStudioAgentGraphQLTransport`, the
  GraphQL-WS event adapter, and the controller in a safe lifetime order.
- Replace custom local-tool glue with
  `NativeBrowserToolDispatcherAdapter`. It maps controller invocations to
  `NativeToolDispatcherV1`, pumps native deadlines from `controller.poll()`,
  and propagates cancellation, context, timing, output, and typed errors.
- `AgentControlLeaseManager::revoke(reason)` is an exact alias of the
  synchronous intent-first `preempt(reason)` path.
- Use `gameModel().containerChanged(...)` instead of a raw subscription for
  the typed metadata feed. The returned `SubscriptionHandle` cancels on
  destruction.
- `gameModel().ensureContainer(input)` and the `bindingKey` list filter require
  Game API 2026-07-24 or newer.
  `marketplace().appListingVersions(vars)` requires Management API 2026-07-24
  or newer. Both operations are validated against their exact published
  endpoint SDL, not the merged schema snapshot.
- Native CLIENT runtimes can use `playerCompute().artifactBytes(...)` and
  `marketplace().clientArtifactBytes(...)` for validated base64 decoding plus
  artifact hash, decimal-string fuel, nullable contract, and version metadata.
- `SaveStateStore::set` and `patch` now update the local cache and mark it
  dirty; call `save()` to persist. `dirty()` and `lastSavedAt()` expose the
  save lifecycle. This corrects the old C++ `patch` behavior, which persisted
  immediately.
- Store revision, queue, error, local-actor, and private-avatar observability
  are real bounded snapshots/counters; lifetime totals are not reset by
  clearing retained rings.

No provider API key or provider client was added. Agent providers remain a
server-side platform concern. See
[`docs/compatibility.md`](docs/compatibility.md) for server requirements.

## Crowdy Studio portable parity

This phase is additive. Native integrations can now use
`client.crowdyStudio()` for the current CrowdyJS v12 project, personal-library,
and common-file GraphQL surface, and
`crowdy::studio::CrowdyStudioController` for the portable headless state
machine.

Key integration points:

- Keep GraphQL `BigInt` ids, revisions, sizes, and fuel values as decimal
  strings.
- Use `CrowdyStudioPatchField<T>::omitted()`, `::null()`, or `::value(...)`
  when updating nullable metadata. `std::optional` alone cannot preserve this
  wire distinction.
- Call `CrowdyClient::poll()` to deliver `*Async` API callbacks on the
  configured dispatcher/game thread.
- Call `CrowdyStudioController::tick()` from the engine loop for autosave,
  offline retry, and visible monitoring refreshes.
- Inject `ICrowdyStudioSynchronizationProvider` for atomic agent patches and
  durable checkpoints, `ICrowdyStudioRuntime` (or
  `CrowdyStudioPlayerComputeRuntime`) for playerCompute execution, and
  `ICrowdyStudioApprovalGate` for exact live/restore approval.
- Live deployment no longer has an unapproved convenience overload. Pass the
  exact plan from `makeDeploymentPlan()` plus an opaque grant issued and
  checked by the agent layer.

The native phase intentionally excludes DOM, Monaco, CSS, browser Rust workers,
and engine rendering. It also does not add a generic GraphQL executor or any
owner/grid/source authority override. Servers must expose the current
`crowdyStudio*` Game API roots; older deployments reject only these new
operations.
