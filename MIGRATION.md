# CrowdyCPP migration notes

## 0.15.0 app-scoped player counts

0.15.0 is additive. Native clients can query
`gameModel().activePlayerCount(appId)` and subscribe with
`activePlayerCountChanged(appId, callbacks)`. Only `FRESH` snapshots are
complete; deduplicate transition events by their decimal-string revision and
requery after reconnects or gaps. These methods require the matching
2026-07-24 Game API generation.

## 0.14.1 Windows replication fix

0.14.1 is a drop-in patch for 0.14.0. Winsock UDP connections are now
configured nonblocking, so `Connection::pump(0)` and manual-pump clients return
immediately when no datagram is available instead of blocking the caller.

## 0.14.0 strict portable-parity release

0.14.0 is not purely additive. It closes every portable gap against the pinned
CrowdyJS 12.0.0 target and adds typed wrappers for current platform SDL
extensions, but it also changes `SaveStateStore::patch` persistence timing and
introduces non-copyable owning runtime types.

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
- `SaveStateStore::patch` no longer performs a network write. It updates the
  local cache and marks it dirty; code that relied on the old immediate
  persistence must call `save()` explicitly. The new `set` method has the same
  local-only behavior. `dirty()` and `lastSavedAt()` expose the save lifecycle,
  and persistence failures now occur at `save()`, not at `patch()`.
- Store revision, queue, error, local-actor, and private-avatar observability
  are real bounded snapshots/counters; lifetime totals are not reset by
  clearing retained rings.
- `AppTokenResponse::gameTokenId` and `AuthResponse::gameTokenId` are decimal
  strings. Use the checked `gameTokenIdInt64()` helper only at the native UDP
  wire boundary. Token route URLs and nullable profile fields now use
  `NullableString`, preserving GraphQL null separately from `""`.
- Native replication startup requires complete `Config::token` metadata.
  Prefer `ReplicationClient::connectWithStatus()` so assignment/socket
  failures are not discarded; the old `connect()` convenience remains.
- `ClientConfig::crypto` is the injection point for non-OpenSSL builds.
  Direct `Connection` construction still requires an `ICrypto`; with OpenSSL
  disabled, omitted crypto fails with `CryptoUnavailable` rather than leaving
  an unresolved symbol.
- `CROWDY_NO_EXCEPTIONS=ON` is a reduced package profile. It does not install
  Compute authoring, Crowdy Studio project API/models/controller,
  Agent/controller, player-host, Game Kit, or `ContainerMirror` headers; use
  the normal package for those layers.

### Runtime ownership and copyability

The new runtime objects own callbacks, protocol state, or host authority and
must not be copied:

- `graphql::SubscriptionHandle`, `graphql::GraphQLSubscriptionClient`,
  `player_host::AgentControlLeaseManager`, and
  `agent::NativeToolDispatcherV1` are move-only.
- `agent::CrowdyStudioAgentController` and
  `agent::CrowdyStudioAgentControllerRuntime` are non-copyable and
  non-movable. Keep the factory result in its returned `std::unique_ptr`
  rather than storing either object in a container that relocates values.

No provider API key or provider client was added. Agent providers remain a
server-side platform concern. See
[`docs/compatibility.md`](docs/compatibility.md) for server requirements.

## Crowdy Studio portable parity

The Crowdy Studio API surface adds
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
