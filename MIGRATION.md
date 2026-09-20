# CrowdyCPP migration notes

## 0.42.0 one HMAC per downlink bundle

Additive. Tracks Buddy v0.30.0 and CrowdyJS 17.6.0 (parity pin). No signature changes.

- `Connection` now sends `CLIENT_CAPABILITIES` (opcode 29) once the session is
  `Connected` and every `Config::advertiseIntervalMs` (15 s). Set
  `Config::advertiseCapabilities = false` to behave as 0.41.
- A Buddy at v0.30.0+ answers with `MESSAGE_BUNDLE_SIGNED` (opcode 30): one HMAC over the
  datagram, members without their own. `wire::verifySignedBundle` checks it (always;
  mismatches count in `Stats::hmacFailures`), `wire::forEachMessage` walks past the tail,
  `Stats::signedBundlesReceived` counts them. Applications that consume `Event`s see the
  same spatial notifications as before, with `containsAuth = 0`.

## 0.41.0 bulk containers

Additive. Tracks cks-game-api v2.6.0 (PRs #346–#349) and CrowdyJS 17.5.0, which
this release's parity gate pins. Every existing method keeps its signature.

- New `GameModelAPI::containerStates(appId, containerIds)` and
  `containerStatesAsync` — the bulk twin of `containerState`: same selection,
  ids as a JSON array, up to 500 per call; ids the app does not hold are
  omitted, duplicates once, input order kept.
- `GmSessionFields` carries `seededContainerCount` (populated on the create
  response only; null on every other read). Container-type selections read
  `scope` (`session` | `app`).
- Generated inputs carry `SeedContainerInput.bindingKey`,
  `SeedContainerTypeInput.scope`, `UpsertContainerTypeInput.scope` and
  `CreateSessionInput.seedFromApp { typeNames, initialState }`; `seed()`,
  `upsertContainerType()` and `createSession()` accept them unchanged.
- Server contract to know: `containers` returns 200 rows when `limit` is
  omitted and refuses above 1,000 (`BAD_REQUEST`); a caller that relied on an
  unbounded list must page. On an `app`-scoped type, `ensureContainer` /
  `createContainer` with a `sessionId` refuse with `CONTAINER_TYPE_APP_SCOPED`.
  Seeded copies of an ended session are dropped by the server after the tier's
  retention window (7 days on every tier); hand-made rows are never purged.
- Parity re-pinned to CrowdyJS 17.5.0 (`81eea4af`); the 45 "covered — CrowdyJS
  17.3.0's snapshot predates it" waivers are gone. `GameModelAPI.sessionChanged`
  joins the async-twin waivers (a subscription handle).

## 0.40.0 the game-model session system

Additive. Tracks cks-game-api PR #319 on top of ck-api v2.3.0. Every existing
`GameModelAPI` session method keeps its signature; the SDK adds what the server
now knows about a session. Numbered 0.40.0 because #100 took 0.39.0 (schema
sync to v2.3.0, parity CrowdyJS 17.3.0) while this was open.

### What is new

- **Roster, admission, capacity, host on `GmSession`:** `admission`
  (`open | locked | closed`), `maxParticipants`, `participantCount`,
  `hostUserId`, `hostTerm`, `revision`, `endedAt`, `endReason`, `createdAt`.
  `createSession` accepts `maxParticipants`, `admission`, `emptyTimeoutSec`,
  `presence`, `idempotencyKey`; `sessions(appId, status, admission,
  hostUserId, limit)` filters and limits.
- **New methods (sync + `Async` twins):** `leaveSession`, `setSessionAdmission`,
  `transferSessionHost`, `endSession` (host or app admin; all accept
  `expectedHostTerm` and `idempotencyKey`), `sessionSnapshot`, `sessionEvents`,
  `sessionInspect` (`manage_apps`), and the typed GraphQL-WS stream
  `sessionChanged(appId, sessionId, afterRevision, callbacks)` delivering
  `GameModelSessionEvent` (`revision`, `kind`, `payloadJson`, ...) in the
  `containerChanged` shape. The contract is the player-count feed's: pull the
  snapshot, apply events above its revision, re-pull on a gap.
- **Reconnection is a rejoin.** `joinSession` on a session you are in returns
  your row with `incarnation + 1` and supersedes any older client of yours; the
  join result is the full roster row. **`leaveSession` requires that
  `incarnation`** -- there is no "leave regardless" -- so a stale client can
  never remove the one that took over (`SESSION_INCARNATION_STALE`).
- **Presence is your actor -- a behaviour change every consumer inherits from
  the server.** A joined participant with no fresh Buddy actor in the app after
  the join grace window (60 s by default) is marked `left` / `presence_expired`,
  and a session nobody has been joined to for longer than its `emptyTimeoutSec`
  (5 min by default; `0` disables) is ended as `abandoned`. A client that only
  speaks GraphQL therefore drops out of a session it never replicates in. Pass
  `actorUuid` on join to bind presence to one specific actor -- your own, in
  Buddy's 32-hex form (`core::toString(WorldSession::actorUuid())`). Do not
  pass the matches kit's channel-ping uuid; it never spawns in Buddy.
- **Opting out: `presence = "none"`.** A session created with
  `sessionInput["presence"] = "none"` is never judged by actor presence
  (`GmSession.presence` reports the mode; `sessionInspect` shows its rows as
  `presence: "none"`). Its roster's only exits are `leaveSession`,
  `endSession` and the empty timeout once everyone has left. Use it for
  turn-based play that talks GraphQL and channel pings and never replicates an
  actor. The mode is fixed at creation. A GraphQL-only session that does
  **not** opt out empties after the grace window and is abandoned after the
  timeout.
- **The `sessionChanged` push is per datacenter; the events table is the
  record.** A revision committed in one region wakes subscribers on that
  region's API replicas; `sessionEvents(appId, sessionId, afterRevision)` (and
  the replay the stream performs on connect) reads the durable rows, so a
  subscriber reconnecting anywhere catches up from the revision it last saw.
- **Error codes** (on `CrowdyGraphQLError::code()`): `SESSION_FULL`,
  `SESSION_LOCKED`, `SESSION_CLOSED`, `SESSION_ENDED`,
  `SESSION_NOT_PARTICIPANT` (the caller is not joined),
  `SESSION_TARGET_NOT_PARTICIPANT` (the user named to `transferSessionHost`
  is not joined), `SESSION_INCARNATION_STALE`, `SESSION_HOST_TERM_STALE`.
- **`kit::MatchesKit` creates its session with `presence = "none"`, and now
  leaves and ends it.** A kit match is GraphQL plus channel pings; its uuid is
  only the channel-message sender id and never spawns in Buddy, so under the
  default mode every player would be expired after the grace window. Because
  nothing expires anybody, the kit owns the roster's exits: new
  **`leave(match, incarnation = nullopt)`** calls `leaveSession` with the
  incarnation the kit remembered from `create()` / `join()` on this instance
  (or the one you pass; `std::invalid_argument` when neither is known) and
  leaves the match channel; **`finish()` now ends the backing session**
  (`endSession`, reason `completed`) after a successful `end_match`, so the
  roster is cleared and the session's events become eligible for retention.
  The result is a `KitMatchFinishResult` whose `sessionEnd` says what happened
  to the session: `"ended"`, `"already_ended"` (a replayed finish), or
  `"forbidden"` (`end_match` admitted the caller but the session did not -- the
  creator who already left, or the app's elected host who is not the session
  host -- so the match is finished, the session is not, and nothing is thrown;
  an app admin can `endSession` it); any other refusal propagates. An emptied
  session that was never finished is abandoned by the empty timeout.
  Otherwise the kit is unchanged: capacity still lives in `MatchMeta` and
  join does not bind an actor. Moving it onto session capacity / admission /
  host is a later, separate change.
- **Schema and parity.** `schema.gql` is synced from the cks-game-api PR #319
  branch rebased on `dev`: the v2.3.0 SDL plus the session delta. The parity
  gate stays pinned to CrowdyJS 17.3.0 (as on `dev`); the session surface is
  CrowdyCPP-ahead and classified `covered-extension` until the pin moves to
  CrowdyJS 17.4.0, at which point those entries go stale and the gate says so.

No removals.

## 0.38.0 a bound GitHub project saves as commits

`CrowdyStudioAPI::saveProject` now follows CrowdyJS 17.0: a `STUDIO` project
still writes through `crowdyStudioProjectSave`; a `GITHUB` project commits
each changed file through `crowdyStudioGitHubPutFile` / `DeleteFile` under
`github.sha` (`expectedCommitSha`) and sends metadata as a project save with
no file bodies. A stale commit or a caller holding an older revision than
the provider last returned is the same `CrowdyStudioRevisionConflictError`
the editor already recovers from. The controller does not know which path
ran.

### What changes for you

- **Nothing if your projects stay in Studio.** Unbound saves are unchanged.
- **A bound project can no longer be saved as Studio file bodies.** Against
  ck-api v2.0 those mutations refuse with `GITHUB_BOUND_USE_CONTENTS`. If you
  were constructing `SaveCrowdyStudioProjectInput` yourself for a project
  whose `source` is `GitHub`, keep doing that — `saveProject` now issues the
  commits. Refresh a bound project that has no `github.sha` before saving.
- **New surface.** `client.crowdyStudioGitHub()` is the typed transport
  (`status`, `layout`, `tree`, `getFile`, `putFile`, `deleteFile`,
  `refresh`, `bind`, `unbind`, `repos`, `connectUrl`). Status / layout /
  tree / file / put / delete / refresh work with an app token; connect /
  repos / bind / unbind need the identity session. Path arithmetic lives in
  `crowdy/studio/github_layout.hpp` (`studioFileToRepoPath` and friends);
  the SDK does not parse `crowdy.json`.
- **Still browser-only.** The hosted Studio settings card
  (`bindGitHubRepo`, `connectGitHub` opening a tab, card busy-state) is not
  on the native controller. A native host that wants bind/unbind calls
  `crowdyStudioGitHub()` itself.
- **`saveProjectAsync` on a bound project is still blocking.** The STUDIO
  path posts one save on the async transport and delivers the callback
  from `poll()`. A GITHUB project runs the same commit loop as
  `saveProject` on the caller's thread and fires the callback before
  returning. The controller uses the sync save. A host that chose
  `*Async` to keep a frame from stalling should treat a bound save like
  `saveProject` until that path is itself async.
- **Also.** `playerComputeSetSwitch` / `playerComputeSwitches` carry
  `listingRef` (LISTING-scope kill), matching CrowdyJS 17.1.0.

Tracks CrowdyJS `17.1.0` (the 17.0 bound-save contract plus 17.1 bundling).

## 0.37.0 outbound sends are bundled by default

`replication::Connection` now packs the messages you send within a short window
into one `MESSAGE_BUNDLE` datagram (`[2]{[u16 LE len][signed message]}...`), the
framing the server has always used for its notifications. Every member is still
a complete, individually HMAC-signed message; only the datagram boundary moved.

### What changes for you

- **Nothing in the send API.** `sendActorUpdate` and friends return the sequence
  number exactly as before. A send is now *accepted* rather than *transmitted*
  when it returns: the datagram leaves when `Config::bundleWindowMs` (default
  1 ms) has passed since the bundle opened, when the next message would not fit
  in 1232 bytes, on `Connection::flushSends()`, at the end of
  `WorldSession::tick()`, before any `*AndWait` starts waiting, and on
  `disconnect()` / reassignment. A lone message is sent unwrapped, so a client
  that sends one message per window puts the same bytes on the wire it always
  did.
- **Manual pump.** With `Config::manualPump` nothing flushes between your
  `pump()` calls except capacity, `flushSends()` and `WorldSession::tick()`.
  Call `flushSends()` at the end of your frame if you pump less often than you
  want datagrams out.
- **Stats.** `Stats::datagramsSent` can now be less than `Stats::messagesSent`;
  `Stats::bundlesSent` counts datagrams that were wrappers (two or more
  members). `messagesSent` advances when a message joins the bundle; the
  datagram/byte counters when the datagram is handed to the kernel. A
  `WouldBlock` on flush keeps the bundle pending for the next attempt and moves
  `sendsDeferred`; a genuine fault drops it and moves `sendsFailed` (once, for
  the datagram) and `Stats::messagesDropped` (once per member that was in it,
  since `messagesSent` had already counted them).
- **Server requirement.** The replication server must unpack client bundles
  (Buddy v0.27.0+). Against an older server, set `Config::bundleSends = false`;
  otherwise any two messages sent within a window are dropped together.
- **Opt out.** `Config::bundleSends = false` is exactly the 0.36 behaviour: one
  datagram per message, transmitted synchronously from the calling thread.

### Also new

- `wire::BundleWriter`, `wire::kBundleHeaderSize`, `wire::kBundleLengthPrefix`,
  `wire::kMaxBundleMembers`, `wire::kMaxBundleMemberSize` (header-only, the
  writer half of `wire::forEachMessage`).
- `UdpSocket::wake()` / `canWake()`: a blocked receive on the net thread can be
  interrupted so a bundle opened mid-wait still leaves within its window.
  (POSIX only; on Windows the net thread bounds its receive wait to one window
  instead.)

## 0.34.0 the Crowdy Agent orchestrator is retired

The Agentic Crowdy Studio agent no longer runs on the server. CrowdyJS 16 docks
the DeepSeek Harness inside the browser Studio pane; it edits the player's
project, runs draft tests and takes screenshots from the page, and spends model
tokens through `POST /v1/model/chat/completions` with the player's own app
token, billed to the player's wallet by default (or the app's org wallet).
The 21 GraphQL roots this SDK drove (`crowdyStudioAgentCreateSession`,
`...SendMessage`, `...Events`, leases, approvals, tool results, history,
budget, ...) are gone from the API, so a 0.33 client loses that surface the
moment the API deploys whether or not it upgrades. There is no native
counterpart: an engine has no Studio pane to dock the harness in.

### Removed

- `crowdy/agent/*` (`CrowdyStudioAgentController`, `CrowdyStudioAgentGraphQLTransport`,
  `AgentToolRegistry`, `NativeToolDispatcherV1`, `NativeBrowserToolDispatcherAdapter`,
  `CrowdyStudioAgentControllerRuntime`, agent schema/types/errors).
- `crowdy/studio/host_adapter.hpp` (`CrowdyStudioHostAdapter`,
  `CrowdyStudioControllerHostAdapter`, the 11 native Studio tools) and
  `crowdy/studio/agent_projection.hpp`.
- `crowdy/player_host/lease_manager.hpp` (`AgentControlLeaseManager`) and
  `crowdy/player_host/control_gate.hpp` (`NativePlayerControlGate`).
- `CrowdyClient::createCrowdyStudioAgentController`.
- `CrowdyStudioIntegrationOptions::{agent, nativeTools, studioHost, controlLeases,
  controlGate, leaseManager, autoInitializeAgent}`;
  `CrowdyStudioIntegration::{agentController, leaseManager, leaseSnapshot,
  controlGate, controlSnapshot, nativeTools, initializeAgent}`;
  `CrowdyStudioIntegration::create` no longer takes an agent runtime factory.
- On `domains::CrowdyStudioAgentAPI`: `session`, `sessions`, `history`,
  `toolDescriptors`, `budget`, `createSession`, `attachClient`, `setMode`,
  `acknowledgeEvents`, `heartbeat`, `sendMessage`, `approveTool`, `rejectTool`,
  `browserToolResult`, `grantLease`, `revokeLease`, `pause`, `resume`,
  `cancelRun`, `closeSession` and their async twins.
- The generated `gen::CrowdyStudioAgentPreemptionReason` enum.
- Parity fixtures `crowdyjs-agent-tools`, `crowdyjs-descriptor-digests`,
  `crowdyjs-preemption-reasons`, `crowdyjs-player-control-gate`,
  `crowdyjs-studio-host-tools`, `crowdy-studio-runtime-sync` and their
  generators; the `${CMAKE_INSTALL_DATADIR}/crowdy/agent` install tree.

### Added

- `CrowdyStudioAgentAPI::providerConsent(appId)`, `setProviderConsent(input)`,
  `modelUsage(appId, limit?)` and async twins
  (`operations/crowdyStudioAgent/CrowdyStudioModel.graphql`).
- `player_host::PreemptionReasonV1` is now a contract-owned enum with the same
  16 wire names in the same order, plus `preemptionReasonFromName`. It used to
  be an alias of the removed generated enum; code that switched over it
  compiles unchanged, code that called `gen::toString` on it should call
  `preemptionReasonName`.
- `CrowdyStudioIntegration::schedule(task)` queues work for the maintenance
  lane (what the removed `studioHost.schedule` did), and `playerHost()` hands
  back the observation-only adapter the engine passed in.

### What to do in a game

- Delete agent wiring. `CrowdyStudioIntegration` still owns the headless Studio
  controller, layout controller and editor bridge; `poll()` pumps the platform
  and `runStudioMaintenance()` is the blocking lane, unchanged.
- Keep `PlayerHostAdapterV1` if your tooling reads observations from it;
  nothing in this SDK dispatches to it any more. The interface keeps
  `dispatch` and `clearAgentIntent` for source compatibility.
- Read agent policy and usage with `client.crowdyStudioAgent()`; record the
  player's provider-data consent with `setProviderConsent` if you drive the
  REST model endpoint yourself.

This is a breaking change in a 0.x line, so it is a minor bump (the same rule
0.20.0 used for `managementUrl`). Requires the ck-api release that carries the
metered model endpoint (first dev release after `v1.98.0`); tracks CrowdyJS
`16.0.0`.


## 0.29.0 nothing runs for an app with no player in it

The schema snapshot moves from 2026-08-28 to the current published SDL, which
carries three platform changes the SDK was documenting incorrectly. Two are
source-compatible; the third refuses a value this SDK's own docs told you to
pass.

### `alwaysOn` is refused

`compute().upsertModule()` with `alwaysOn: true` now fails with `BAD_REQUEST`.
The field is deprecated server-side, always reads `false`, and is dropped from
the module fragment this SDK selects, so `ComputeModuleFields` no longer returns
it.

`compute.hpp` told you to "set alwaysOn=true for world simulation that must run
without connected players". That is now the one thing it cannot do. Delete the
argument.

### Modules and scheduled work require a player

A compute module ticks only while its app has at least one player connected
anywhere in the fleet, and stops when the last one leaves. Scheduled automations
and `gm_timers` follow the same rule, with one difference worth knowing:

- **Automations** due while the app is empty are **skipped** and rescheduled from
  the moment a player returns. Missed runs are never made up.
- **Timers** whose deadline passes while the app is empty **wait** and fire on
  return. Late, not lost.
- **`event` and `manual` triggers** are unaffected — something already asked.
- **Synchronous `compute().invoke()`** is unaffected — a request, not a tick.

**What to change in a game.** Make scheduled work idempotent in *elapsed time*
rather than assuming a cadence: advance the world by `now - lastTick`, and store
expiries as timestamps rather than remaining-tick counters. The Game Kit headers
for `worldsim`, `combat` and `economy` said their automations run "with no client
online"; that was true and is not, and all three now say so along with how to
write against it. A blueprint that assumes a cadence stalls silently while nobody
is playing, which is the failure mode to design out.

### Reservations split, and mean something different

`App` gains `reservedUdpBytesPerSec` and `reservedGraphqlOpsPerSec`, and the app
query now selects both. `reservedEgressBytesPerSec` is deprecated and returns the
UDP value for one release. Reserving one dimension does not reserve the other.

`admin().setAppReservedThroughput()` is unchanged in shape and changed in
meaning. Three things a reservation is **not**, all of which it either was or
appeared to be before:

1. **Not a ceiling.** It obliges the platform to keep that much capacity
   provisioned for you and does not cap what you may send. Traffic above the
   reserved rate is metered, not refused.
2. **Not a data allowance.** The monthly fee buys capacity, not volume, and is
   charged *in addition to* metered usage. Reserving 5 MB/s does not make the
   first 5 MB/s free.
3. **Not the way to lift the free-tier cap.** Unfunded free apps are shaped at
   roughly 1 MB/s; funding the org wallet or enabling auto-billing lifts that.
   Until 2026-09-01 a reservation did double duty as the bypass, which is why the
   old schema description said "bypasses the ~1 MB/s rate limit".

### `Connection::Stats` byte counters are not your bill

No behaviour change; a documentation one worth reading if you have ever compared
the two. `bytesSent` and `bytesReceived` are a local diagnostic and will not
reconcile against an invoice:

- Billing counts **egress only**. `bytesSent` is traffic leaving the CLIENT,
  which is ingress to the platform and is not counted toward Aggregate Data
  Volume at all.
- The platform measures at **its** network interface, including IP and UDP
  headers. These counters count the datagram payload.

Use them for backpressure and diagnosis; use `admin().appUsageSummary()` for what
you are charged.

### Also

The rate-card field docs carried a stale worked example of 15c per GiB; the
published SDL now says 19c, matching the shipped card.


## 0.25.0 rate-limit refusals carry how long to wait

The server sends `extensions.retryAfterMs` on a `RATE_LIMITED` refusal.
`GraphQLErrorDetail` parsed a fixed set of extension keys and kept no raw
`extensions` handle, so that one was unreachable: a caller could tell that it had
been refused for asking too often and could not tell for how long. CrowdyJS
callers read `error.extensions` directly and never had this gap.

### Added

- **`GraphQLErrorDetail::retryAfterMs`** — `std::optional<std::int64_t>`,
  populated from `extensions.retryAfterMs` when it is a JSON number.

It is optional deliberately, and the `retryable` field beside it is the wrong
model to copy. `retryable` defaults to `true` because the server contract says an
absent value means "trying again is reasonable". There is no such default for a
duration: **`0` means retry now and absent means the server named no wait**, and
collapsing the two would turn silence into a busy loop. A value that is present
but not a number reads as absent.

```cpp
if (const auto& wait = error.retryAfterMs) {
  scheduleRetryIn(std::chrono::milliseconds(*wait));
} else {
  scheduleRetryWithLocalBackoff();
}
```

**Read it as a deadline, not as an interval to reuse.** On the invoke rate limit
the server computes what REMAINS of a fixed window rather than a fixed backoff,
so a second refusal inside the same window carries a smaller number, and a value
cached from an earlier refusal will be too long.

### Fixed

Subscriptions and HTTP requests were parsing the GraphQL `errors` array through
two independent copies of the same code, and they had drifted: the subscription
copy never read `blame`, so the same refusal arriving over the websocket lost its
attribution. Both call one internal `readGraphQLError` now.

**If you branched on `blame` and treated its absence on a subscription error as
"unattributed", that branch will now see the real value** — which is the
documented contract, but it is a behaviour change on the websocket path.

No wire change, and no other public type moved.

## 0.24.0 sign and verify with a pre-keyed MAC

Every datagram was signed with OpenSSL's one-shot `HMAC()`, which builds a
context and re-imports the 64-octet token for each call. The token changes only
on refresh, so that setup was repeated needlessly on every send and on every
inbound notification, and it was not a small part of the cost: it was
essentially all of it. Encoding a datagram takes 4 ns; signing it took 1225.

Measured on the builder, with the numbers and the method in
[benchmarks/README.md](benchmarks/README.md):

- encode + sign: 1225 ns -> **319 ns**
- verify a notification: 1276 ns -> **337 ns**
- a 200-entity frame through `Connection::sendActorUpdate`: about
  **1.5x** less CPU

No wire change. Same key, same message, same tag — the golden vectors are
unmoved and a test signs against them through the new path, including from
several threads at once.

### Added

- **`crowdy::core::IMac`** — a MAC bound to one key, reusable across messages,
  computing over parts without concatenating them.
- **`ICrypto::makeHmacSha256(key)`** — returns a pre-keyed `IMac`, or nullptr.

**Implementing it is optional.** The base class returns nullptr and every
caller falls back to `hmacSha256`, so an engine-injected `ICrypto` written
before this release keeps compiling and behaving identically; it simply does
not get the speedup. Engines that bind their own crypto should implement
`makeHmacSha256` to pick it up — it is the single largest CPU win available on
the replication path.

- **`spatialHmac`, `encodeLongSpatial`, `encodeChannelMessage` and
  `verifyLongSpatial`** take an optional trailing `const IMac*`. Existing calls
  compile and behave exactly as before.

### Changed

- `Connection` builds the pre-keyed MAC once per token, rebuilds it on
  rotation, and reads the token id, key and MAC under a single lock where it
  previously took the same mutex twice per send.

### Considered and rejected

Batching writes with `sendmmsg` measured between 1.00x and 1.04x, including
with loopback delivery removed from the measurement, because the kernel does
the same per-datagram work either way and only the syscall boundary is
amortised. No batch API was added. The evidence is in
[benchmarks/README.md](benchmarks/README.md) so the decision can be revisited
on hardware where syscalls cost more.

## 0.23.0 send backpressure is not a socket failure

The UDP send path treated every short write as `Errc::SocketError` and threw
the OS error code away. On a non-blocking socket a full kernel send buffer is
`WSAEWOULDBLOCK`/`EAGAIN` — ordinary backpressure — so a client emitting a large
population in one frame silently lost outbound updates and logged them as
socket faults. It was reported from a 200-entity, 10 Hz Unreal harness: several
hundred sends "failing" inside three milliseconds, which no broken socket does.

Nothing about the wire changed. No datagram, framing, or HMAC behavior is
affected.

### Added

- **`Errc::WouldBlock`** — transient: nothing was sent, the socket is healthy,
  retry shortly. Appended to the enum, so every existing value is unmoved.
- **`ReplicationConfig::socketSendBufferBytes`** (default `1 << 20`) — the
  `SO_SNDBUF` hint, matching the `socketRecvBufferBytes` that already existed.
  The send side previously ran on whatever the OS default happened to be.
- **`Connection::Stats::sendsDeferred`** and **`sendsFailed`** — saturation and
  faults counted apart. `sendsDeferred` rising under load is expected;
  `sendsFailed` rising is not.
- **`UdpSocket::nativeHandle()`** — the underlying descriptor, for diagnostics
  and reading socket options back. The socket still owns it.

### Changed

- **`UdpSocket::send` returns `Errc::WouldBlock` for a full send buffer.**
  Callers treating any non-`Ok` as fatal will now see it. Requeue and retry
  instead of dropping; a permanently full socket needs a bounded queue.
- **An exhaustive `switch` over `Errc` needs a `WouldBlock` case.** `errcName`
  handles it, and unmatched values still fall through to `"Unknown"`.
- **The POSIX send is now non-blocking** (`MSG_DONTWAIT`), matching Winsock and
  the receive path in the same file. Previously a saturated buffer blocked the
  calling thread on POSIX — a frame hitch in a game loop — where it now returns
  `WouldBlock` promptly. Portable callers must handle that return.
- **`UdpSocket::open` takes a fourth argument**, `sendBufferBytes`. Direct
  callers of the socket (rather than `Connection`) need updating; `<= 0` keeps
  the OS default.
- **`LocalActorStore` no longer records a deferred send as an error.**
  `status()` stays out of `Error` for a merely busy socket, and the update
  remains dirty so the next tick retries it.
- **A heartbeat that failed to send is no longer recorded as sent.** It
  previously advanced the heartbeat timestamp regardless of the result, so a
  heartbeat that never left the box banked the whole interval and presence
  could lapse while every counter still read healthy. This applies to any
  failure, not only backpressure.

## 0.20.0 one origin, and endpoints that can move (breaking)

Tracks CrowdyJS 14.1.0. 0.17.0 unified the two APIs behind one server but kept
the two-endpoint shape in the client; this release removes it, and adds the
machinery a client needs when the one endpoint it holds stops answering.

### Removed

- **`ClientConfig::managementUrl`** and **`ClientConfig::managementGraphqlEndpoint`**.
  Pass `httpUrl` (and `wsUrl`). For a per-game client that is the app's OWN
  datacenter endpoint from `mintAppToken`, because that is where its shards live.
- **`CrowdyClient::managementClient()`** and **`CrowdyClient::managementSubscriptions()`**.
  Use `graphqlClient()` and `subscriptions()`.
- **`MarketplaceAPI`** and **`CrowdyStudioAgentAPI`** take one GraphQL client.
  Studio moderation, policy, usage and operator roots are separated from player
  operations by the permissions they require, not by endpoint.
- **`gen::GraphQLEndpoint`** and the per-domain **`endpointFor()`**, along with
  the `schema.management.gql` / `schema.game.gql` snapshots that produced them.

CrowdyJS throws a `TypeError` when a caller passes a removed option. C++ gets
the stronger version for free: a removed struct field is a compile error, so
there is no build in which the old spelling is silently ignored.

### Added

- **`ClientConfig::discoveryUrl`** — the shared origin (multivalue DNS over
  every datacenter's balancer), returned as `discoveryUrl` by `mintAppToken`,
  `refreshAppToken`, `exchangePortalCode` and `gameClientBootstrap`. Set it and
  a client whose endpoint dies can ask where to go next.
- **`ClientConfig::rediscover`** / **`rediscoverAfterFailures`** (default 3) —
  the re-discovery hook, coalesced and never fatal.
- **`discovery()`** — `appDiscovery`, answering where an app is served BEFORE
  login, so a client can start on the shared origin and move before it
  authenticates.
- **Datacenter redirect** — `WRONG_DATACENTER` moves the client (HTTP, WS and
  UDP together) and retries once; `APP_UNAVAILABLE` raises
  `CrowdyAppUnavailableError` and carries no endpoint, on purpose.
- **`SERVER_DRAINING`** — a control-only `udpNotifications` subscription, so a
  native client gets the same advance warning a browser client already had.

### Environment variables

`CROWDY_E2E_API_URL` replaces `CROWDY_E2E_MANAGEMENT_URL` in the e2e harness.
The old name is still read: it names the same origin now, and a harness still
setting only the old name would have made every suite exit 77 — a skip, which
reads as a pass rather than as a failure.

## 0.17.0 unified API (breaking)

Tracks CrowdyJS 13.0.0: the platform merged the Management and Game APIs into
ONE server on the shared database (galaxy then; PostgreSQL + Citus since
2026-08-04). The committed schema snapshots were resynced from the unified SDL,
and the surfaces the platform retired are removed. (The two snapshots this
release kept, `schema.management.gql` and `schema.game.gql`, were themselves
retired in 0.20.0 — see below.)

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
- **Endpoints**: `managementUrl` and `httpUrl` may be the same origin;
  configuring both remains supported here, and the two-token model is
  unchanged. (0.20.0 removed `managementUrl` outright.)

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
