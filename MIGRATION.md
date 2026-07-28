# CrowdyCPP migration notes

## 0.17.0 unified galaxy API (breaking)

Tracks CrowdyJS 13.0.0: the platform merged the Management and Game APIs into
ONE server on the shared galaxy database. The committed schema snapshots are
resynced from the unified SDL (`schema.management.gql` and `schema.game.gql`
are now the same schema), and the surfaces the platform retired are removed:

- **`client.admin().environments()` removed.** Dedicated customer
  environments no longer exist; every app runs on the shared platform, and
  infrastructure provisioning moved to the separate infra-control-plane
  service. The `EnvironmentsAPI` class, its accessor, and the
  `operations/environments/` documents are gone.
- **`client.operator_()` reduced to the platform compute ceilings**
  (`computePlatformCeilings` / `setComputePlatformCeilings`). Environments,
  change orders, secrets, releases, audit, and operator-user listing moved to
  the infra-control-plane service.
- **`client.admin().usage()`**: the per-environment rollups
  (`environmentSummary` / `environmentByApp` / `orgByEnvironment`) are gone;
  `appSummary`, `appGraphqlOperations`, `playerPulse`, and the org/app
  projections stay.
- **`client.admin().billing()`**: the per-environment capacity tier catalogs
  (`buddyBillingTiers` / `graphqlBillingTiers` / `postgresBillingTiers`) are
  gone; wallets, budgets, and transactions stay.
- **Endpoints**: `managementUrl` and `httpUrl` may now be the same origin;
  configuring both remains supported and the two-token model is unchanged.

Everything game-client, replication, kit, session, Crowdy Studio, agent, and
player-host is unchanged — the merged schema is a superset for those surfaces.

## 0.16.0 native Crowdy Studio integration

0.16.0 changes the public source and installed-library ABI. CrowdyCPP remains
pre-1.0: rebuild consumers and engine wrappers, and do not load 0.16 libraries
behind binaries compiled against 0.15 headers.

- Prefer `CrowdyClient::createCrowdyStudioIntegration(options)` for an
  engine-owned Studio. The returned non-copyable `CrowdyStudioIntegration`
  owns the project/runtime adapters, controller, editor bridge, concrete Studio
  host, native dispatcher, optional Agent runtime, layout controller, lease
  manager, control gate, and wallet adapter in destruction-safe order.
- Implement `ICrowdyStudioEditorAdapter` over in-memory buffers. Its callbacks
  intentionally provide no `CrowdyClient`, filesystem, shell, DOM, transport,
  or raw GraphQL authority. Existing direct-controller integrations can remain,
  but must own every borrowed provider for the controller's full lifetime.
- Persist pane state through `ICrowdyStudioLayoutStorage`, or retain
  `InMemoryCrowdyStudioLayoutStorage` for session-only state. CrowdyCPP does not
  choose a filesystem, registry, browser local storage, or process-global
  settings service. `crowdy/studio/layout.hpp` remains available in the reduced
  no-exception install.
- Replace opaque diagnostic views with `CrowdyStudioDiagnostic`. Diagnostics
  now carry target-relative path/range, severity, source, message, and optional
  rustc code. The string setter and `*DiagnosticTexts()` accessors remain
  compatibility helpers.
- Wallet state is optional read-only observation. The client integration
  installs `CrowdyStudioPlayerWalletProvider` by default; set
  `observePlayerWallet = false` or inject `walletProvider` to override it.
  Wallet failures clear the snapshot and never block editing, saving, testing,
  or deployment.
- `CrowdyStudioControllerHostAdapter` is the closed 11-tool Studio host.
  Potentially effectful calls validate session, epoch, context, lease,
  cancellation, and approval at the final boundary. Do not replace it with a
  generic host call, raw operation executor, or transport callback.
- Supply `playerHost` to let the integration own one exact
  `AgentControlLeaseManager` shared by native dispatch and
  `NativePlayerControlGate`. Forward existing keyboard, pointer, movement,
  background, death, permission, admission, context, and controlled-entity
  events through the gate. It observes takeover intent; it does not synthesize
  or consume engine input.
- `CrowdyStudioIntegration::poll()` (and compatibility spelling `tick()`) is
  nonblocking. It drains platform/Agent callbacks and native deadlines only.
  Autosave, monitoring HTTP, compile polling/sleep, and scheduled Studio host
  effects run from `runStudioMaintenance()`. Serialize that potentially
  blocking lane with all controller/editor access; never call it concurrently.
- DOM, Monaco, CSS, splitters, browser workers/VFS, renderer chrome, and
  browser input plumbing remain intentional browser exclusions. Engines own
  equivalent presentation and sandbox implementations; 0.16 adds no hidden
  DOM/filesystem authority.

Approved checkpoint restore is not implied by project-save access. It remains
available only when an independently authorized
`ICrowdyStudioSynchronizationProvider` and exact
`ICrowdyStudioApprovalGate` are both injected. The published GraphQL schema
does not expose a generic synchronization or restore root.

## 0.15.1 installed-package compatibility patch

0.15.1 changes no runtime API. It updates external-consumer verification to
request the current `0.15` CMake compatibility line.

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
- Consume `CrowdyStudioDiagnostic` from the authoritative/local state vectors.
  Existing string producers can keep using the
  `setLocalDiagnostics(std::vector<std::string>)` overload; existing text-only
  views can use
  `localDiagnosticTexts()` or `authoritativeDiagnosticTexts()`.
- Inject `ICrowdyStudioSynchronizationProvider` only when the host has an
  independently authorized durable atomic-patch/checkpoint service,
  `ICrowdyStudioRuntime` (or
  `CrowdyStudioPlayerComputeRuntime`) for playerCompute execution, and
  `ICrowdyStudioApprovalGate` for exact live/restore approval.
- Optionally inject `ICrowdyStudioWalletProvider` as the last controller
  argument. `CrowdyStudioPlayerWalletProvider` adapts only the viewer-scoped
  `PlayerWalletAPI::balance()` read; failures leave authoring operational.
- Live deployment no longer has an unapproved convenience overload. Pass the
  exact plan from `makeDeploymentPlan()` plus an opaque grant issued and
  checked by the agent layer.

There are no generic checkpoint-list, atomic-patch, or approved-restore roots
in the published GraphQL SDL. Without an injected bridge those calls now fail
with `CrowdyStudioCapabilityUnavailableError` instead of suggesting that
ordinary project-save GraphQL authority is sufficient. Durable
`AgentCheckpoint` event metadata can be mapped with
`crowdyStudioCheckpointEventFromAgentV1()` and scope-checked by
`ingestCheckpointEvent()`; restore still requires a separate exact approval
grant and bridge.

The native phase intentionally excludes DOM, Monaco, CSS, browser Rust workers,
and engine rendering. It also does not add a generic GraphQL executor or any
owner/grid/source authority override. Servers must expose the current
`crowdyStudio*` Game API roots; older deployments reject only these new
operations.
