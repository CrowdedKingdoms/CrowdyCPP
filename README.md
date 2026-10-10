# CrowdyCPP

The official portable C++ SDK for **Crowded Kingdoms**. CrowdyCPP gives native
games typed clients for auth, GraphQL HTTP and WebSocket APIs, and — unlike
the browser-first [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) SDK —
a **native UDP replication client** that speaks the
[Replication API wire protocol](https://docs.crowdedkingdoms.com/replication-api/intro)
directly to the replication servers, with zero-copy binary framing and no
GraphQL proxy in the hot path.

CrowdyCPP is designed from first principles to be:

- **Portable.** Standard C++20, CMake, Linux/Windows/macOS. No engine types,
  no framework assumptions. Every platform dependency (HTTP, crypto, clock,
  logging, allocation) sits behind a small interface you can replace.
- **High performance.** Steady-state zero heap allocation on the replication
  path, zero-copy datagram parsing, lock-free queues between the network and
  game threads, batched socket I/O, and no exceptions on hot paths.
- **Embeddable.** Usable directly by a native game, and equally designed to be
  wrapped by engine-specific SDKs — see
  [Wrapping CrowdyCPP in engines](#wrapping-crowdycpp-in-engines) for the
  Unreal Engine plan.

CrowdyCPP mirrors the [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS)
API surface (same domains, same two-token model, same error codes) so the
[platform docs](https://docs.crowdedkingdoms.com) and examples translate
directly. Where CrowdyJS routes realtime traffic through the Game API's
GraphQL UDP proxy (a browser constraint), CrowdyCPP opens a raw UDP socket and
implements the
[wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats)
and [HMAC scheme](https://docs.crowdedkingdoms.com/replication-api/hmac)
natively.

**v0.60.0: voice helpers, opcode 140 and voxel edits in WorldSession, wide voxels.**
`crowdy/media/voice_frames.hpp` is an optional voice payload convention shared with CrowdyJS 18.7.0
(a 10-byte header in the audio payload, `VoicePacketizer`, `VoiceJitterBuffer`; both SDKs replay
one fixture), and `-DCROWDY_WITH_OPUS=ON` (default OFF) adds a libopus wrapper
(`crowdy/media/opus.hpp`). `WorldSessionConfig::onVoxel` hears every voxel update after
`chunks()` merged it, and `onGenericSpatial` forwards opcode 140, which the session used to drop.
`ChunkStore` keeps an edit the one-byte 16³ grid cannot hold (a type outside 0-255, a position
outside 0-15) in `ChunkData::overlay` instead of truncating it, and `voxelTypeAt` returns
`std::int16_t`. No wire change. See [MIGRATION.md](MIGRATION.md).

**v0.59.0: the input log.** `client.inputLog().sessions(appId, first, after, filter)` and
`messages(appId, gameTokenId, first, after, filter)` (each with an `Async` twin) read the client
inputs recorded for an app with replay logging on (`App.replayLoggingEnabled`, now selected on every
app read and set with `admin().apps().update`). Game plane: the app-scoped client. A player reads
their own; `manage_apps` reads every session. A messages page can be short, or empty, while
`pageInfo.hasNextPage` is true, so keep paging until it is false. Pinned to CrowdyJS 18.6.0. See
[MIGRATION.md](MIGRATION.md).

**v0.58.0: channel messages limited by distance.** `Connection::sendRangedChannelMessage(channelId,
uuid, payload, origin, maxDistance)` publishes to a channel so that only members with a live actor in
the connection's app within `maxDistance` chunks of `origin` receive it (straight-line distance
between chunk coordinates, boundary included, 0 to 2147483647; Buddy v0.35.0). Members receive the
ordinary channel notification through `Handlers::channelMessage`. Pinned to CrowdyJS 18.5.0. 0.57.0's
terms and age gate is unchanged. See [MIGRATION.md](MIGRATION.md).

**v0.56.0: chunk loads apply recorded voxel edits.** A hub's or mod's `world.set_voxels`,
`updateVoxel` and realtime voxel updates land in a chunk's edit log, never in its stored
`voxels`; since ck-api `dev/v2.33.0` the chunk reads return each of them as a `voxelStates`
entry. `ChunkStore::ensureAround`'s one `getChunksByDistance` now selects the entries and puts
each over the grid, so a block a hub placed is there after a reload; before, it vanished.
`IChunkSource::chunksAround` reports them in `StoredChunk::voxelStates`. Still pinned to CrowdyJS
18.1.0. See [MIGRATION.md](MIGRATION.md).

**v0.55.0: parity with CrowdyJS 18.1.0, open grids and where a connect token may go.**
`gameApps().setOpenPermissions` / `openPermissions` open a grid to every player with access
(cks-game-api #436): a zone everyone may build in must grant `update_voxel_data` itself now.
`exec().connect` and `connectAsDeveloper` dial only a gateway `execGatewayRefusal` passes (`wss:`
under an `https:` Game API, on the platform's estate; loopback for a loopback Game API), and a
gateway's refusal of the connect token (`HTTP 401`, ck-exec 0.10.0+) is `Denied` again, with its
reason where the transport can read it; `ExecConnection::lastFailure()` says why an attempt
failed. Also carries #134's `<windows.h>` `far` / `near` fix. See [MIGRATION.md](MIGRATION.md).

**v0.54.0: seams for wrapping the native core.** `Config::onEventsReady` wakes an event loop when notifications are waiting (at most once per `poll()` cycle), `IChunkSource` and `IHostElection` let `ChunkStore` and `WorldSession` run over injected durable services (`WorldSessionServices`) instead of a `CrowdyClient`, and `WorldSessionConfig::onText` forwards proximity text the session used to drop. Additive; no wire or parity change (still CrowdyJS 18.0.4). [CrowdyPy](https://github.com/CrowdedKingdoms/CrowdyPy), the Python SDK, binds this release. See [MIGRATION.md](MIGRATION.md).

**v0.53.0: parity with CrowdyJS 18.0.4, chunk write-backs the server refuses.** One chunk
the server refused, or one that kept failing, no longer stops every other chunk from being
written back. `ChunkStore` sends a refused write-back (FORBIDDEN, a validation error,
`retryable: false`, HTTP 400/403/404/413/422) once and drops it, retries one that can clear up
to 5 attempts (0.7/1.4/2.8/5.6 s) and drops it, and reports both through
`onWriteBackFailed`; `flush()` returns a `ChunkFlushResult` that lists what it dropped, and
`pruneBeyond` evicts a refused chunk. The app queries and `createApp` / `updateApp` select
`wildernessWritesOpen` (ck-api `dev/v2.30.0`). See [MIGRATION.md](MIGRATION.md).

**v0.52.0: parity with CrowdyJS 18.0.3, the P3 W5 client security review.** A player takes
back consent to a CLIENT half and trust in its author: `exec().revokeClientModConsent(appId,
modId)` and `revokeAuthorTrust(appId, gridId, authorId)` (and their `…Async` twins; ck-api
`dev/v2.28.0`). The exec connect token is percent-encoded in the gateway URL, as CrowdyJS
encodes it. CrowdyJS's page
now holds a CLIENT half to rules of its own (`grid_permission_check` answers only for the four
code-permission keys, a half's spatial and channel sends go out under a uuid the page derives,
`voxel_set` takes voxels 0-15 of type 0-255, a chunk named a second way is refused, and the glue
caps what it copies out of a module); a native engine that runs CLIENT halves answers its
sandbox's host calls by the same rules. See [MIGRATION.md](MIGRATION.md).

**v0.51.0: the SDK is for normal clients.** It wraps nothing only a super-admin or a platform
operator can call, and is designed for the production environment; org-admin features stay. Gone:
`client.operator_()` (`creditOrgWallet`), `users()` `paginated` / `listConnection` /
`setSuperAdmin` / `setOperator` / `setEarlyAccessOverride` / `updateType` / `forceLogout`,
`admin().organizations().setStatus`, `admin().apps().setVisibility` (use `update` with
`visibility`), `admin().payments()` `checkouts` / `checkoutsConnection` / `paymentEvents` /
`paymentEventsConnection` (`myCheckouts` stays), and `crowdyStudioAgent()` `platformPolicy` /
`setPlatformPolicy` / `setOperatorAppKill`. `admin().quotas().set` refuses a rule naming neither an
app nor an org before any request. Platform tooling calls the API directly. Parity pinned to
CrowdyJS 18.0.1. See [MIGRATION.md](MIGRATION.md).

**v0.50.0: the legacy engines are gone.** ck-exec (`client.exec()`) replaced the Game API's game
model and its automations, Studio compute, player compute and the player model, and the Game API
deleted them (ck-api v2.27.0 on dev), so their C++ surface is removed: `client.gameModel()`,
`client.compute()`, `client.playerModel()`, `client.playerCompute()` (its CLIENT path too: a
mod's CLIENT half replaces it), the Game Kit's blueprints, `deploy()`, engines and model-backed
kits (`makeKit` keeps `social()`; `kit/wire.hpp` and `kit/actions.hpp` stay), the session layer's
`ContainerMirror`, model lint, the quarantine fields of `GraphQLErrorDetail`, the marketplace's
player-code listings and grid attachments, the player wallet's WASM policies and the operator's
compute ceilings. Tier features moved to `admin().appAccess()`. Crowdy Studio's SERVER target is
a ck-exec mod and its CLIENT target that mod's CLIENT half (`CrowdyStudioModRuntime`). Parity
pinned to CrowdyJS 18.0.0. See [MIGRATION.md](MIGRATION.md).

**v0.49.0: ck-exec CLIENT halves (dev-tier preview).** A mod can carry a CLIENT half: browser WASM
built from a `crowdy-client-sdk` crate, which its grid serves to visitors who consent to its
capability hash or trust its author. `client.exec()` adds `modClientBuild`, `modClientDeploy`,
`modClientDelete`, `gridClientMods`, `consentClientMod`, `trustAuthor` and `modClientArtifact`, and
`modClientArtifactBytes`, which hands a native sandbox the module only after checking its SHA-256
against the served digest, its CLIENT ABI (`kExecClientAbiVersion`) and its capability summary.
The SDK runs no WASM itself. Builds and listings select the new CLIENT fields, so they need ck-api
v2.24.0. `LocalActorStore` resends a failed send on the next tick. Parity pinned to CrowdyJS
17.14.0.

**v0.48.0: ck-exec observability (dev-tier preview).** `client.exec()` adds `endpointStats` /
`endpointStatsAsync` (calls per endpoint by outcome, with latency), the `flow` filter on `logs`
(`ExecLogsQuery::flow`) and `flow` on every log line, and `manifestJson` on `versions`.
`ExecReply::rateLimited()` and `retryAfterMs()` expose the gateway's per-player call limit (120
calls per 10 s per app on a host, refused `Busy` with "rate limited: ...; retry in N ms"); the
SDK never retries `Busy`, so wait `retryAfterMs()` before calling again. Parity pinned to
CrowdyJS 17.13.0.

**v0.47.0: ck-exec mods (dev-tier preview).** `client.exec()` adds players' code on grids they
own: `modStarter`, `modBuild` / `waitForModBuild`, `modDeploy`, `modSetEnabled`, `modDelete`,
`mods`, `myMods`, `modLogs`, the marketplace without payments (`modPublish`, `modListings`,
`modUnpublish`, `modInstall`), and for developers `appMods`, `modSwitches` and the kill ladder
`modSetSwitch`. A mod is the node type `execModType(name)` keyed by the grid id. Parity pinned to
CrowdyJS 17.12.0.

**v0.46.0: ck-exec builds (dev-tier preview).** `client.exec()` adds `starters`, `build`,
`buildStatus` and `waitForBuild`, so a game builds its hubs on the platform, and `deploy` takes
the build's id with types naming their `crate`. Parity pinned to CrowdyJS 17.11.0.

**v0.45.0: ck-exec operations (dev-tier preview).** `client.exec()` adds
`connectAsDeveloper` (studio tools and admin endpoints, as `Caller::Developer`), guest `logs`,
`instances`, `versions`, `status`, `activateVersion` (a rollback) and `setEnabled` (the kill
switch). Parity pinned to CrowdyJS 17.10.0.

**v0.44.0: ck-exec (dev-tier preview).** `client.exec().connect(appId, {.nodeType, .key})`
asks the Game API for an execution host and opens a WebSocket to its gateway; the
`ExecConnection` it returns calls hubs and spokes (`call`, `callRaw`), subscribes to their
topics, pings, and reconnects with subscriptions renewed when the host goes away or a call
is answered `Moved`. Payloads are MessagePack: `graphql::Json::toMsgpack` /
`Json::fromMsgpack`. `exec().deploy(...)` deploys an app's nodes. Callbacks run on
`poll()`. Parity pinned to CrowdyJS 17.9.0.

**v0.43.1: an open circuit says why.** `GraphQLErrorDetail::cause` is `watchdog_timeout` when a breaker opened on watchdog kills, and `PlayerFaultCode` includes `CIRCUIT_OPEN`. Parity pinned to CrowdyJS 17.8.0.

**v0.43.0: grids.** `client.grids()` mints grid-scoped tokens and manages grid channels,
and `gameModel().sessions(..., gridId)` lists the games hosted inside a grid (DN-10:
grid scope is app scope intersected with grid confinement). Parity pinned to CrowdyJS
17.7.0.

**v0.42.1: CLIENT_CAPABILITIES actually leaves the client.** 0.42.0 built opcode 29
and `encodeLongSpatial` refused it, so no datagram was sent and a Buddy never
switched to `MESSAGE_BUNDLE_SIGNED`. 0.42.1 accepts 29 in `isLongSpatialLayout`
and sends the advertisement. Same CrowdyJS 17.6.0 pin as 0.42.0.

**v0.42.0: one HMAC per downlink bundle, parity CrowdyJS 17.6.0 (Buddy v0.30.0).**
`Connection` sends `CLIENT_CAPABILITIES` (29, `wire::ClientCapability::kAll`) once the
session is `Connected` and every `Config::advertiseIntervalMs` (15 s) after
(`Config::advertiseCapabilities`, default on; `Connection::sendCapabilities()` for tests).
A Buddy at v0.30.0+ then sends `MESSAGE_BUNDLE_SIGNED` (30): the bundle framing, members
with `containsAuth = 0`, one trailing HMAC over the datagram. `wire::verifySignedBundle`
checks it (always, one per datagram; a mismatch counts in `Stats::hmacFailures`),
`wire::forEachMessage` walks past the tail, and `Stats::signedBundlesReceived` counts them.
Older Buddies ignore 29 and keep the per-member form. Mirrors CrowdyJS 17.6.0.

**v0.41.0: bulk containers, parity CrowdyJS 17.5.0.** `GameModelAPI` gains
`containerStates` / `containerStatesAsync(appId, containerIds)` (the bulk twin of
`containerState`, up to 500 ids); `GmSessionFields` carries `seededContainerCount`
and the container-type selections read `scope`; the generated inputs carry
`SeedContainerInput.bindingKey`, `SeedContainerTypeInput.scope`,
`UpsertContainerTypeInput.scope` and `CreateSessionInput.seedFromApp`, so
`seed()`, `upsertContainerType()` and `createSession()` accept them as-is.
`containersWhere` states the page contract (default 200, max 1,000). Tracks
ck-api v2.6.0; schema re-synced; the 45 stale session waivers are gone.

**v0.40.0: the game-model session system.** `GameModelAPI` gains
`leaveSession`, `setSessionAdmission`, `transferSessionHost`, `endSession`,
`sessionSnapshot`, `sessionEvents`, `sessionInspect` and the typed
`sessionChanged` GraphQL-WS stream; `GmSession` carries admission, capacity,
host / host term, revision and end fields, and the join result is the roster row
(`state`, `incarnation`, `actorUuid`, ...). `leaveSession` REQUIRES the
incarnation the join returned, and presence is the player's Buddy actor — a
session created with `presence = "none"` opts out, which is what
`kit::MatchesKit` does — see `MIGRATION.md`. Schema synced to cks-game-api PR
#319 on top of v2.3.0; parity
pinned to CrowdyJS 17.3.0 with the session surface classified as covered until
the pin moves to 17.4.0.

**v0.39.0: schema synced to ck-api v2.3 (Studio GitHub `repositorySelection`; Crowdy
Games hosting roots), parity CrowdyJS 17.3.0.** The Crowdy Games hosting surface
(CrowdyJS 17.2.0: `client.hosting`, `EmbeddedHost`, the `*HostedGame*` /
`*GamePublish*` roots) and the Studio card's "Create repository on GitHub"
(17.3.0) are browser exclusions — a native client has no browser bundle to
publish and no tab to open. Schema sync and codegen only; no native API change.

**v0.38.0: bound GitHub projects save as commits, parity CrowdyJS 17.1.0.**
`CrowdyStudioAPI::saveProject` commits each changed file on a `GITHUB`
project (`crowdyStudioGitHubPutFile` / `DeleteFile` under `expectedCommitSha`)
and keeps `STUDIO` saves on the project revision. `client.crowdyStudioGitHub()`
is the typed transport; `crowdy/studio/github_layout.hpp` maps Studio files
onto the repository layout the API resolved. The hosted settings card stays
a browser exclusion. See [MIGRATION.md](MIGRATION.md).

**v0.37.0: outbound message bundling.** `replication::Connection` packs the
messages a client sends within a short window into one `MESSAGE_BUNDLE`
datagram — the same framing the server has always used on the downlink, now
accepted on the uplink by Buddy v0.27.0+. On by default (`Config::bundleSends`),
window `Config::bundleWindowMs` (1 ms), forced with `Connection::flushSends()`
and at the end of every `WorldSession::tick()`. A lone message is sent
unwrapped, so a client sending one message per window puts the same bytes on
the wire as before. `Stats::datagramsSent` may now be less than
`messagesSent`; `Stats::bundlesSent` counts the wrappers. `bundleSends = false`
is the 0.36 behaviour. See [MIGRATION.md](MIGRATION.md). Parity CrowdyJS 17.1.0.

**v0.36.0: schema synced to ck-api v2.0 (Crowdy Studio GitHub is optional; a
bound repository is the working tree), parity CrowdyJS 17.0.1.** `CrowdyStudioProject`
carries `source` / `githubOwner` / `githubRepo` / `githubBranch` / `githubSha`;
`playerComputeDeploy` takes `projectId` (+ `commitSha`) and no longer accepts
`sourceFilesJson`; `crowdyStudioGitHubLayout` / `Refresh` / `DeleteFile` are new
and were browser exclusions until 0.38.0 wrapped the transport and bound
save. `crowdyStudioGitHubSetAutosave` is gone. Schema sync and codegen only.

**v0.35.0: micro-USD wallet fields, parity CrowdyJS 16.2.0.** Org and player
wallets carry `balanceMicrousd` / `holdsMicrousd`, transactions `amountMicrousd`
/ `balanceAfterMicrousd` (ck-api v1.100.x lossless billing ledger; the cents
fields stay, deprecated). Schema sync and codegen only.

**v0.34.0: the Crowdy Agent orchestrator is gone, parity CrowdyJS 16.0.0.**
The Studio agent is now the in-browser DeepSeek Harness that CrowdyJS docks
beside the web editor and that spends tokens through the metered REST
`/v1/model` endpoint. The 21 `crowdyStudioAgent*` session/run/lease/tool
roots left the API, and with them this SDK's `crowdy/agent/*`, the Studio host
adapter and agent projection, the player-host lease manager and control gate,
`createCrowdyStudioAgentController`, and the agent fields of
`CrowdyStudioIntegrationOptions`. `CrowdyStudioAgentAPI` keeps policy, usage
and operator controls and gains `providerConsent` / `setProviderConsent` /
`modelUsage`. `player_host/` is observation-only. Breaking; see
[MIGRATION.md](MIGRATION.md). Schema synced to the ck-api that carries the
model endpoint; pin is CrowdyJS `16.0.0`.

**v0.33.0: paid commerce off the public schema, parity CrowdyJS 15.12.0.**
`domains::Marketplace` drops `purchaseGrid`, `createGridListing`,
`gridListings`, `setListingPricing`, renew/top-up/refund, seller
onboarding/payouts, and the risk queue. Free publish / acquire / install /
consent / claim stay. Schema synced from the ck-api that unregistered those
fields.

**v0.32.0: nearbyGrids + bindPolicyJson, parity CrowdyJS 15.9.0.** Tracks
ck-api `v1.93.0`. `gameApps().nearbyGrids` lists overlapping grids (id +
bounds only). Studio ops select `bindPolicyJson` on container types
(authoring surface; no live Titan Assault policy writes). Schema synced
to that API; pin is CrowdyJS `15.9.0` at the `dev` merge of that release.

**v0.31.0: async refresh can name the current server.** `portal().refreshAsync(ip4, port)`
is the async twin of the server-aware blocking refresh: it sends
`refreshAppToken(currentServer)` so a proactive refresh can keep the Buddy.
`CrowdyClient::refreshGameplayTokenAsync` uses that overload when the quiesced
connection still has an assignment. Parity was CrowdyJS 15.7.0.

**v0.30.0: webcam video and the server-announced departure.** Tracks CrowdyJS 15.5.0
(Buddy `v0.25.x`, Game API `v1.87.x`). `Connection::sendVideo` sends one fragment and
`sendVideoFrame` fragments a whole encoded frame (`include/crowdy/media/video_frames.hpp`:
the 6-byte header, at most 16 fragments of 1117 bytes, `VideoFrameAssembler` for the
receiving side -- byte-identical to CrowdyJS, seven shared fixture cases).
`Handlers::video` and `Handlers::actorLeft` are dispatched from opcodes 144 and 145;
`WorldSession` forwards audio/video through `WorldSessionConfig::onAudio` / `onVideo`
(it used to swallow both) and removes an announced actor from `actors()` at once,
firing `onLeave` and `onActorLeft`, instead of after the `staleAfterMs` reap. Video is
gated by `use_video_chat` (bit 9), opt-in on the app's tier. The e2e harness gains
`CROWDY_E2E_OWNER_PASSWORD` so it can sign in a tier's real owner.

**v0.29.2: a native client keeps its Buddy across a token refresh.** `refreshToken`
now names the replication server the client is on (`refreshAppToken(currentServer)`,
Game API v1.83.7+) and the connection keeps its socket when the API reports the new
token `authorizedOnCurrentServer`, re-assigning only when it does not. Before, every
30-minute refresh left the client on a Buddy that had never heard of its new token —
mute until the (opt-in) watchdog re-placed it. `ISessionProvider::refreshToken(const
Assignment*)` is a new virtual with a default that forwards to the old form, so
existing providers compile unchanged (and re-assign after every refresh, as before).

**v0.29.1 ships the default origin its tier actually declares.** The generated
`default_origin.hpp` on the `dev` and `test` lines said `prod` — carried there by promotion
merges, which is the hazard the generated-file-plus-gate arrangement exists to catch. So a
consumer of a `dev` or `test` tag that constructed a client with no explicit origin dialled
**production**. That is the one direction worse than shipping no default at all: it is what
an unconfigured consumer gets, and it fails in a way that looks like the product rather
than like a misconfiguration. Fixing the branch was only half of it — this SDK is consumed
as source, so the tag IS the artifact and nothing reached a consumer until this was cut.
Nothing else changed; parity stays pinned to CrowdyJS 15.4.0, because no surface moved.

**v0.27.0 ships a default origin:** a client constructed with no explicit origin
now dials the public CK API for the tier this build was released for, from the
generated `crowdy/default_origin.hpp` — `kDefaultHttpOrigin`, `kDefaultWsOrigin`,
`kDefaultHost` and `kDefaultTier`. The header is written by the operator tooling
from one per-tier declaration and a gate refuses it when it names the wrong tier,
so the default cannot quietly drift from the host that actually serves. An
explicit origin still wins; nothing that configures one changes behaviour.

**v0.26.0 drops the dev auth bypass:** `devLogin` and `devLoginAsync` are gone,
along with `requestLoginLink`'s `devToken` selection, because ck-api deleted all
three — the mirror cannot offer a field the server does not have. Sign in with
`login(email, password)` or create an account with `registerUser`, which is
`register` renamed because C++ cannot name a method after a keyword. Parity is
pinned to CrowdyJS 15.0.0. Breaking for any caller that used the bypass; there
was no other way to remove it.

**v0.24.0 signs with a pre-keyed MAC:** the replication path signed every
datagram with a one-shot HMAC that re-imported the 64-octet token each time,
which was essentially the entire per-datagram CPU cost — encoding is 4 ns,
signing was 1225. `ICrypto::makeHmacSha256()` now supplies a reusable keyed
`IMac`, cutting sign to 319 ns and notification verify to 337 ns, about 1.5x
less CPU for a 200-entity frame. No wire change. Implementing the new method is
optional: providers that do not fall back to the old path unchanged. Numbers
and method in [benchmarks/README.md](benchmarks/README.md).

**v0.23.0 tells backpressure apart from a broken socket:** a full kernel send
buffer now returns the new transient `Errc::WouldBlock` instead of
`Errc::SocketError`, so a client outrunning its send buffer under burst load is
no longer indistinguishable from a genuine socket fault. The POSIX send is
non-blocking to match Winsock and the receive path, `SO_SNDBUF` is configurable
through `ReplicationConfig::socketSendBufferBytes`, and `Connection::Stats`
separates `sendsDeferred` from `sendsFailed`. See [MIGRATION.md](MIGRATION.md) —
callers that switch exhaustively over `Errc` need a new case.

**v0.22.0 builds on Windows again:** line endings are normalised through
`.gitattributes` so a Windows checkout is byte-reproducible, the generated
headers are regenerated from an LF checkout, and the CMake build compiles and
tests under MSVC. Nothing about the API changed — this is the 0.21.0 surface,
buildable on the platform where 0.21.0 was not.

Released from the `dev` branch as `dev/v0.22.0`. Since 2026-08-12 this repo
carries `dev`, `test` and `prod`, and a release tag names the branch it was cut
from; a bare `vX.Y.Z` tag is the retired convention. **v0.20.0 and v0.21.0 were
never tagged on the remote** despite a commit saying they were, which is why the
tier-tag release path and its gates exist. That is history now: releases have
been tagged from `<tier>/vX.Y.Z` since 0.25.0, and as of 2026-08-21 `dev`,
`test` and `prod` are all **0.26.0**. `main` was fast-forwarded to `prod` and
then **deleted** the same day, with every other repo's — a public branch a
consumer can mistake for the release had already cost a wasted defect report.
Derive the current one from
`git tag --sort=v:refname` rather than from this paragraph.

**v0.21.0 invoke fault attribution:** a failed `gameModelInvoke` now reports
whose fault it was and whether repeating it can work, on both channels the
server uses: `fault { code blame retryable }` in band on a rejection, and
`extensions.blame` on the thrown overload refusal. The parity pin is unmoved at
CrowdyJS 14.1.0 `90f4b7bb2562d007aa62d01d4b21abdb76923e9b`, and the release gate
reported zero portable gaps, unclassified differences, and stale
classifications against it.

The schema sync this needed advanced the published SDL by several server
releases, which added ten root fields. They are wrapped here rather than
waived, so the SDK still reaches every field its schema declares. This release
also builds and tests with MSVC again: the embedded agent fixture had outgrown
the 65535-byte limit on a single string literal, and is now emitted as adjacent
literals that concatenate to the identical constant.

**v0.20.0 one origin, movable endpoints:** the release gate reported zero
portable gaps, unclassified differences, and stale classifications against
CrowdyJS 14.1.0 at
`90f4b7bb2562d007aa62d01d4b21abdb76923e9b`. This is not a claim that the
implementations are identical: the generated matrix retains reviewed native
equivalents and browser-only exclusions. Production Agentic Studio uses typed
GraphQL-WS durable events, reconnect gap-fill, and lifetime-safe controller
construction; the native player-host dispatcher has an exact
`IAgentBrowserToolDispatcher` bridge. Native Studio adds the complete
editor/layout/host/control assembly through
`createCrowdyStudioIntegration()`, with nonblocking `poll()` and an explicit
potentially-blocking maintenance lane. See the
[compatibility matrix](docs/compatibility.md) and
[0.16 migration notes](MIGRATION.md).

Older minors (0.7–0.19) are in [MIGRATION.md](MIGRATION.md). The live data
plane is **PostgreSQL + Citus** via `cks-game-api`; `cks-management-api` is
retired. There is one GraphQL origin since 0.20.0.

## Layout

```text
include/crowdy/          public headers
  core/                  bytes, endian, result, clock, logging, allocator interfaces
  wire/                  zero-copy wire codec + HMAC framing (header-only)
  graphql/               GraphQL HTTP + WebSocket transports, JSON, errors
  replication/           native UDP replication client
  session/               world session layer (actors, chunks, inboxes, host)
  kit/                   Game Kit (social helpers, wire codecs, optimistic actions)
  media/                 webcam video fragments and voice payloads (header-only), optional Opus
  player_host/           typed player observations and their schemas
src/                     implementation
include/crowdy/generated/  committed codegen output (operations + enums)
operations/              GraphQL operation documents (codegen input)
schema.gql               the published API SDL snapshot (codegen input)
scripts/                 schema sync + codegen (Node, maintainers only)
tests/                   unit tests (ctest) + env-gated e2e tests
benchmarks/              micro + end-to-end benchmarks
```

## Build

```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev   # Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # unit tests: no network required
```

A clean external clone builds offline: the schema snapshot (`schema.gql`) and
the generated code (`include/crowdy/generated/`) are committed. Only maintainers run the
schema sync / codegen scripts (see [Schema refresh](#schema-refresh-and-codegen)).

Dependencies (all replaceable through interfaces):

| Dependency | Used by | Replaceable via |
|---|---|---|
| libcurl | default HTTP transport | `crowdy::graphql::IHttpTransport` |
| libcurl 8.13+ WebSocket APIs | optional default GraphQL subscriptions | `crowdy::graphql::IWebSocketTransport` |
| OpenSSL (libcrypto) | HMAC-SHA256 | `crowdy::core::ICrypto` |
| yyjson (vendored) | JSON parse/serialize | internal only, not on the UDP path |
| libopus (optional, `CROWDY_WITH_OPUS=ON`) | `crowdy/media/opus.hpp` | any codec: the voice helpers carry opaque frames |

The wire and replication layers depend only on BSD/Winsock sockets and the
`ICrypto` interface — no libcurl, no JSON.

The default HTTP transport works with older supported libcurl releases. The
optional default WebSocket backend requires libcurl 8.13+ because older
releases do not provide the fragmented-message semantics it needs. CMake
feature-detects that backend; if support is absent (or
`CROWDY_WITH_CURL_WEBSOCKETS=OFF`), the SDK builds with a clear no-default
fallback and `makeCurlWebSocketTransport()` returns null; injected engine
transports continue to work on Linux, macOS, and Windows. The factory also
checks that the linked libcurl is 8.13+ and actually advertises both `ws` and
`wss`, since some distributions expose the APIs while compiling those
protocols out.

`CROWDY_WITH_OPUS=ON` (default OFF) builds `crowdy/media/opus.hpp`'s libopus wrapper as the
target `crowdy_opus`, which `CrowdyCPP::crowdy` then links and which defines `CROWDY_HAS_OPUS`.
libopus is found through its CMake package (vcpkg, `-DCMAKE_PREFIX_PATH`), else pkg-config
(`libopus-dev`, Homebrew), else a plain header and library search; the installed package finds
it the same way. Configuring it ON without libopus fails. The default build needs neither Opus
nor network, and without the option the header declares only `kOpusAvailable = false`.

`CROWDY_NO_EXCEPTIONS=ON` creates a reduced strict `-fno-exceptions` package:
core GraphQL outcomes, auth/portal, replication, non-authoring domains, and
session stores remain available. The independent Crowdy Studio pane-layout
header remains available. Crowdy Studio project models/API/controller,
Agent/controller, player-host and Game Kit headers are not installed because
their validation contracts throw. Blocking GraphQL failures return an invalid `Json`; use
`*Async` callbacks for typed details. Injected transports must not throw
across the SDK boundary.

## Quick start

```cpp
#include <crowdy/crowdy.hpp>

int main() {
  const std::string appId = "42";  // GraphQL BigInt stays a decimal string.

  // 1) Identity client on the shared entry origin.
  crowdy::CrowdyClient identity(crowdy::ClientConfig{
      .httpUrl = "https://ck.example.com",
  });
  // devLogin was REMOVED in 0.26.0 (ck-api deleted it on every tier).
  // registerUser is `register` renamed, because C++ cannot name a method after a keyword.
  auto login = identity.auth().login("player@example.com", "correct-horse-battery");

  // 2) Mint an app-scoped token and build the per-game client.
  auto minted = identity.portal().mintAppToken(appId);
  // httpUrl is the app's OWN datacenter (that is where its shards are);
  // discoveryUrl is the shared origin to fall back to if it stops answering.
  crowdy::CrowdyClient game(crowdy::ClientConfig{
      .httpUrl = minted.gameApiUrl,
      .discoveryUrl = minted.discoveryUrl,
  });
  game.setToken(minted.token);

  // 3) Connect the native replication client (assigns a server, installs the
  //    UDP session, waits for session-ready).
  const auto appIdInt = crowdy::graphql::parseBigInt(minted.appId);
  const auto tokenIdInt = minted.gameTokenIdInt64();
  if (!appIdInt || !tokenIdInt || !minted.gameApiUrl.has_value()) return 2;
  crowdy::replication::Config repl{
      .appId = *appIdInt,
      .token = {.token = minted.token,
                .gameTokenId = *tokenIdInt,
                .expiresAtEpochMs = 0},  // Parse minted.expiresAt in production.
  };
  // 4) Install receive handlers and join the world.
  crowdy::replication::Handlers handlers;
  handlers.actorUpdate = [](const crowdy::replication::SpatialNotification& u) {
    // u.uuid, u.chunk, u.payload (span over the datagram — copy if you keep it)
  };
  auto connected = game.replication().connectWithStatus(repl, handlers);
  if (!connected.ok()) return 3;
  auto conn = connected.connection;
  conn->sendActorUpdate({.chunk = {0, 0, 0},
                         .uuid = myActorUuid,
                         .payload = poseBytes,
                         .distance = 8,
                         .decay = crowdy::wire::DecayRate::Exponential});

  // 5) Pump notifications from your game loop (or use the owned-thread mode).
  while (running) {
    conn->poll();
    if (conn->state() == crowdy::replication::ConnState::Failed ||
        conn->state() == crowdy::replication::ConnState::Closed) {
      return 4;
    }
  }
}
```

The full lifecycle (tokens, server assignment, reconnect commands, token
refresh) is documented in
[Authenticate and assign](https://docs.crowdedkingdoms.com/replication-api/authenticate-and-assign)
and handled by `crowdy::replication` automatically.

### Generic GraphQL subscriptions

`GraphQLSubscriptionClient` implements `graphql-transport-ws` for portable
push APIs. It normalizes an HTTP(S) API URL to one WS(S) `/graphql` endpoint,
authenticates with the shared token in `connection_init`, bounds UTF-8 JSON
messages, and replays active operations after capped jittered reconnects.

```cpp
crowdy::graphql::GraphQLSubscriptionCallbacks callbacks;
callbacks.onNext = [](crowdy::graphql::GraphQLSubscriptionOutcome next) {
  if (next.ok()) {
    // Read next.data on the game thread.
  }
};
callbacks.onError = [](crowdy::graphql::GraphQLSubscriptionError error) {
  // Branch on error.kind / error.code; terminal auth, app-scope, and stale
  // client-epoch failures are not reconnected.
};
callbacks.onReconnect = [](crowdy::graphql::GraphQLReconnectInfo replay) {
  // Optionally start a durable gap-fill query before replayed events arrive.
};

// `yourFeed` stands for an application-specific subscription root.
auto subscription = game.subscriptions().subscribe(
    "subscription Feed($appId: BigInt!) { yourFeed(appId: $appId) { id } }",
    crowdy::graphql::JVal::object({{"appId", appId}}), "Feed",
    std::move(callbacks));

while (running) game.poll();  // all callbacks are delivered here
// subscription.cancel() is explicit; destruction also cancels.
```

The published Game API schema's only subscription root is `udpNotifications`, which
CrowdyCPP replaces natively (it replicates over UDP itself); ck-exec pushes arrive on an
`ExecConnection` subscription instead. The generic client remains for
application-specific subscriptions.
See
[GraphQL WebSocket examples](docs/graphql-websocket.md).

## Sub-clients at a glance

CrowdyCPP mirrors CrowdyJS's domain layout. Game-client surface (end-user,
app-scoped token):

| Sub-client | What it does |
|---|---|
| `client.auth()` | Sign-in: `login` / `registerUser` (email + password), magic link, social/OIDC. Passwords: `requestPasswordReset` / `resetPassword`, `changePassword`, and `setInitialPassword` for an account created by magic link or a social provider. Log out, linked identities. **No dev bypass** — removed in 0.26.0. |
| `client.users()` | `me`, `updateGamertag`, profile reads. |
| `client.session()` | Token store, restore, set/get token. |
| `client.portal()` | `mintAppToken`, `refresh`, PKCE portal entry for cross-origin handoffs. |
| `client.serverStatus()` | `gameClientBootstrap(appId)` — version info, spatial limits. |
| `client.chunks()`, `client.voxels()`, `client.actors()`, `client.avatars()`, `client.state()` | World data reads + writes. |
| `client.host()` | Host election reads + actor liveness heartbeat. |
| `client.teleport()` | Teleport requests. |
| `client.channels()`, `client.teams()` | Messaging channels and app-scoped teams. |
| `client.exec()` | **ck-exec:** an app's server code as hubs and spokes — `connect` / `ExecConnection` (calls, subscriptions, reconnects), `starters` / `build` / `deploy`, the operations (`logs`, `instances`, `versions`, `endpointStats`, `activateVersion`, `setEnabled`), players' mods on grids they own (`mod*`, the kill ladder `modSetSwitch`), and a mod's CLIENT half (`modClientBuild` ... `modClientArtifactBytes`, which checks the module against its digest for a native sandbox). |
| `client.marketplace()` | Player-authorized grid claims (`claimGridChunk`, `releaseClaimedGrid`, ownership claims, requests and invites) and studio moderation of player code (admission queue, listing administration, claim policy). |
| `client.crowdyStudio()` | Caller-owned Crowdy Studio projects and reusable files: list/get/create, revision-fenced atomic saves (STUDIO file bodies; GITHUB commits via `saveProject`), metadata/file updates, archives, personal library, curated common files, and copy-by-value imports. |
| `client.crowdyStudioGitHub()` | Bound-repository transport on the same session: `status` / `layout` / `tree` / `getFile` / `putFile` / `deleteFile` / `refresh` (app token), plus `connectUrl` / `repos` / `bind` / `unbind` (identity session). Path helpers in `crowdy/studio/github_layout.hpp`. |
| `client.gameApps()` | App grids, first-class ownership (`ownership` / `assignOwnership` / `transferOwnership`), and grid runtime-permission administration. |
| `client.subscriptions()` | Generic `graphql-transport-ws` operations with RAII cancellation, reconnect/replay notification, and game-thread delivery from `poll()`. |
| `client.crowdyStudioAgent()` | Agentic Studio policy, provider-data consent and metered model usage. The agent itself runs in the player's browser (CrowdyJS 16 `dsh`) against the REST `/v1/model` endpoint; see [native agent API](docs/native-agent-api.md). |
| `client.replication()` | **Native UDP** replication: connect/assign, spatial sends, notifications, channel publish, single-actor messages, heartbeats. |
| `crowdy::session::WorldSession` | SDK-managed game state: your actor with a fixed-Hz send loop, remote-actor registry with staleness + interpolation history, chunk/voxel cache, inboxes, host tracking — see [the session layer](#the-session-layer-data-structures-that-do-the-bookkeeping). |
| `crowdy::kit::makeKit(client, appId)` | Game Kit: parties, guilds and chat over teams and channels (`social()`); `crowdy/kit/wire.hpp` (engine pose codec, event parsers) and `crowdy/kit/actions.hpp` stand alone — see [Game Kit](#game-kit-social-helpers-and-wire-codecs). |

Studio-admin surface (privileged; drive with an org/admin token from a trusted
context): `client.admin().organizations() / apps()` (including player-code
admission policy) `/ appAccess() / billing() /
payments() / quotas() / usage() / sharedEnvironment()`. The SDK carries
org-admin features but nothing only a super-admin or a platform operator can
call; platform tooling calls those fields directly. The SDK never relaxes
server-side authorization — these are typed wrappers; the caller still needs
the right token and permission.

## Headless Crowdy Studio

`client.crowdyStudio()` targets the Game API with the app-scoped player token.
All ids and revisions remain decimal strings, project source is filtered by
the server's `(app, owner, project)` tuple, and nullable metadata patches use
`CrowdyStudioPatchField<T>` so omit, explicit null, and value stay distinct.
The API exposes no raw operation executor.

For an engine-owned editor, construct the controller from injected interfaces:

```cpp
// The SERVER target is the grid's ck-exec mod; the CLIENT target is that mod's
// CLIENT half, which the engine's artifact runtime runs.
crowdy::studio::CrowdyStudioModRuntime runtime(
    game.exec(), &engineArtifactRuntime, [&game] { game.poll(); });
crowdy::studio::CrowdyStudioController studio(
    {.appId = appId, .gridId = gridId},
    game.crowdyStudio(), runtime, engineCrypto, engineClock,
    &durableSynchronization, &agentApprovalGate);

studio.initialize();
studio.updateFile(crowdy::studio::CrowdyStudioTarget::Server,
                  "src/lib.rs", source);
studio.tick();  // engine-loop autosave/retry/monitor pump
```

`CrowdyStudioState::authoritativeDiagnostics` and `localDiagnostics` are typed
`CrowdyStudioDiagnostic` values with target-relative ranges, severity, source,
message, and optional rustc code. `parseRustcDiagnostics()` accepts bounded
human/JSON rustc output; the string overload of `setLocalDiagnostics()` and
the `*DiagnosticTexts()` helpers remain for older engine views.

Wallet balance is optional observation, not authoring authority. Inject
`CrowdyStudioPlayerWalletProvider` (the read-only adapter over
`PlayerWalletAPI::balance()`) as the controller's final constructor argument
to populate `state.wallet` whenever the visible Usage surface refreshes.
Wallet read failures clear that optional snapshot and do not block editing,
saving, compilation, or deployment.

Runtime actions are revision-bound:

- draft/live plans must name the exact complete project target set;
- live plans additionally bind pairing preference and the canonical
  full-project content hash, then pass through the injected agent approval
  gate before compilation;
- full-stack publication compiles CLIENT, builds the SERVER crate as the
  grid's mod (`modBuild`, then `modDeploy` when the build succeeds), enables
  it (`modSetEnabled`), then starts the exact CLIENT artifact through the
  engine runtime; a mod has no client pairing;
- Invoke calls one of the mod's endpoints (`state` by default) over an exec
  connection, and Logs are its `ctx.log` lines (`CrowdyStudioLogLine`);
- checkpoint restore likewise requires the external agent layer's opaque,
  exact approval grant;
- `state.runtimeSync` explicitly distinguishes never-run, running-saved,
  running-stale, and stopped state.

The published GraphQL schemas have durable agent checkpoint events, but no
generic checkpoint-list, atomic-patch, or approved-restore root.
`ICrowdyStudioSynchronizationProvider` is therefore an explicit
host/orchestrator bridge, not a generated GraphQL adapter. Missing bridge
operations throw `CrowdyStudioCapabilityUnavailableError`. Integrations can
convert a scope-fenced `AgentCheckpoint` with
`crowdyStudioCheckpointEventFromAgentV1()` and feed the metadata to
`ingestCheckpointEvent()`; that observation never creates restore authority.
Approved restore still requires both the injected durable bridge and the exact
external approval gate.

The synchronization and runtime interfaces are intentionally server-free in
unit tests. They do not grant grid permissions or source visibility: Game API
ownership, target write/run permissions, and admission checks still execute on
every mod and CLIENT-half call. The installed Studio parity fixtures pin the common
CrowdyJS runtime projection while retaining native content-hash, module, and
pairing bindings. See [MIGRATION.md](MIGRATION.md) for source-behavior and
runtime-ownership notes.

## Native player-host observation

`crowdy/player_host/` is the typed observation contract a game can implement
so tooling can read the controlled player and their surroundings:
`PlayerHostAdapterV1`, `GameObservationV1`, the schemas that validate them and
the closed preemption vocabulary. Since 0.34.0 it is observation only. The
lease manager, control gate, native tool dispatchers and Studio host adapter
that executed commands for the Crowdy Agent orchestrator went with that
orchestrator: the Studio agent now runs in the player's browser, only
observes, and has no native counterpart.

`CrowdyStudioIntegration` owns the headless controller, layout controller and
editor bridge. Integration `poll()` is a nonblocking platform pump; autosave,
monitoring HTTP and compile polling run only from the explicit serialized
`runStudioMaintenance()` lane, which also drains work queued with
`schedule()`.

World coordinates, distances, health values, fuel, revisions, and other
contract values that may exceed a native or JSON number remain decimal
strings. The typed schemas reject non-canonical forms before an adapter runs.
See the [native player-host notes](docs/native-player-host.md) and
[native Studio integration guide](docs/native-studio-integration.md).

## The native replication client

`client.replication()` implements the public
[Replication API](https://docs.crowdedkingdoms.com/replication-api/intro):

- **Connect** = mint/hold an app token → `serverWithLeastClients` on the Game
  API (which installs your UDP session server-side) → wait for session-ready →
  signed UDP traffic to the returned host and client port.
- **Sends**: actor updates, voxel updates, audio, text, client events, generic
  spatial, single-actor messages, channel publishes, and idle heartbeats — all
  HMAC-SHA256 signed per the
  [HMAC guide](https://docs.crowdedkingdoms.com/replication-api/hmac).
  Sends are packed into `MESSAGE_BUNDLE` datagrams by default: each signed
  message joins the pending bundle and the datagram leaves when
  `Config::bundleWindowMs` (1 ms) has passed, when the next message would not
  fit (1232 bytes), on `Connection::flushSends()`, at the end of
  `WorldSession::tick()`, before an `*AndWait`, and on disconnect. A lone
  message goes out unwrapped. `Config::bundleSends = false` restores one
  datagram per message. Needs a replication server that accepts client
  bundles (Buddy v0.27.0+).
- **Receives**: bundle unpacking, per-notification HMAC verification
  (constant-time), typed dispatch, and error frames correlated by sequence
  number.
- **Lifecycle**: automatic app-token refresh before expiry, verified
  `COMMAND_RECONNECT` handling (reassign within the grace period), and a
  silent-drop watchdog (traffic going out with nothing coming back triggers
  reassignment — see
  [Troubleshooting](https://docs.crowdedkingdoms.com/replication-api/troubleshooting)).

Two integration modes:

- **Owned net thread** (default): the SDK runs a receive/send thread and hands
  you notifications through a lock-free SPSC ring; you drain it with `poll()`
  from your game thread.
- **Manual pump**: no SDK threads at all. You call `pump()` from your own
  network thread (or tick) and `poll()` from the game thread. This is the mode
  engine wrappers use.

Performance characteristics: after connect, the steady-state path performs no
heap allocation (pooled datagram buffers), no copies on parse (payloads are
spans into the receive buffer until you copy them), and no exceptions.
Notification callbacks run on the thread that calls `poll()` — never on the
network thread.

Sends never block. When a burst outruns the kernel send buffer, `send` returns
`Errc::WouldBlock`: the datagram was not transmitted, the socket is healthy,
and the caller should requeue and retry shortly. That is a different outcome
from `Errc::SocketError`, which means a real fault — branch on the two rather
than treating any non-`Ok` as fatal, or a saturated client silently loses
outbound updates at exactly the moment it can least afford to. `Connection`
counts them separately (`stats().sendsDeferred` vs `stats().sendsFailed`), and
`ReplicationConfig::socketSendBufferBytes` (default 1 MiB) sizes the buffer
that absorbs the burst — raise it for clients replicating many entities per
frame.

## The session layer: data structures that do the bookkeeping

The replication client moves datagrams; `crowdy::session::WorldSession` turns
them into game state. Every store below is a structure that multiplayer games
otherwise hand-write (and debug) themselves — using them means your first
playable build is a render loop over ready-made state instead of weeks of
netcode bookkeeping. One connection feeds all of them; you call
`session.tick()` once per frame and read plain snapshots (single-threaded by
design, so reads never lock).

| Structure | What it replaces | How it makes you faster |
|---|---|---|
| `LocalActorStore` (`session.self()`) | your presence loop | Joins the world, re-sends state at a fixed Hz with send-on-change dedup, periodic keyframes, and cheap idle heartbeats so presence never lapses; tracks `lastAck()` from self-echoes. You just `setState(bytes)` from the game loop — or `moveTo(chunk)` on boundary crossings for an immediate send. |
| `RemoteActorStore` (`session.actors()`) | everyone-else tracking | Self-filtered registry keyed by actor uuid with staleness reaping, `onJoin`/`onLeave`/`onUpdate` callbacks, and a per-actor sample history (state + server timestamp pairs) ready for interpolation/extrapolation. Render directly from `list()`. |
| `RemoteActorLane` (`actors().lane("mobs", ...)`) | per-kind actor lists | Filtered sub-registries (players vs mobs vs vehicles) so each kind is classified once at ingest — no per-frame re-scanning or re-decoding of the full registry. |
| `ChunkStore` (`session.chunks()`) | terrain sync | Chunk/voxel cache: bulk `ensureAround()` hydration from the durable store (with each chunk's recorded voxel edits, its `voxelStates`), realtime merge of incoming voxel edits, optimistic `setVoxel()` (applies locally, replicates, queues persistence), worldgen write-back via `seed()`, `pruneBeyond()`/`flush()` for streaming worlds, `onWriteBackFailed()` for write-backs it dropped (refused, or out of attempts), and `voxelTypeAt()`/`voxelStateAt()` reads for meshing/collision. |
| `EventRouter` (`session.events()`) | RPC dispatch switch | Routes typed client/server events (`[u16 eventType][state]`) to per-type handlers and retains `lastEvent(type)` — your gameplay events become `events().on(kDoorOpened, ...)` instead of a hand-rolled switch over payload bytes. |
| `Inbox` (`channelInbox()` / `directInbox()`) | chat/message queues | Bounded queues for channel and direct messages with `drain()`, non-consuming `messages()`, `onMessage` callbacks, and channel discovery — plus `send()` helpers back through the connection. |
| `ErrorStore` (`session.errors()`) | "why was that send rejected?" | Correlates server error frames (sequence-numbered, uint8 wrap) with the *kind* of send that used that sequence, so a permission denial points at "your voxel edit", not a bare error code. |
| Host tracking (`amIHost()` / `onHostChanged`) | election polling | Heartbeats host eligibility on a cadence and caches the elected host with a change callback; gate host-only simulation without writing the polling loop. |
| `SaveStateStore` / `AvatarStateStore` | persistence plumbing | Byte-level caches over the durable save/avatar surfaces with explicit `load()`/`save()`; base64 stays at the wire boundary, your code sees bytes. |
| `PodCodec<T>` / `UnrealPose` | wire layout code | Your replicated state as a packed struct: the struct layout *is* the little-endian wire layout (static-asserted), with the 88-byte Unreal-compatible pose included. No serializer to write, nothing to keep in sync. |
| `IUuidStore` (memory/file) | identity persistence | Persist your actor uuid across restarts so remote registries treat you as the same actor. |

Under the hood these sit on the same primitives the hot path uses — the
lock-free SPSC ring between network and game thread, pooled fixed-size
buffers, and zero-copy parsed views — so the convenience layer does not trade
away the performance story.

What the session has no store for it forwards through `WorldSessionConfig`: `onAudio`,
`onVideo`, `onText`, `onGenericSpatial` (opcode 140, an app-defined spatial payload; 0.60.0),
`onActorLeft`, and `onVoxel`, called for every inbound voxel update after `chunks()` has merged
it, with its sender and state blob, for a game that keeps its own world (0.60.0). `ChunkStore` is a
helper for 16×16×16 chunks with one byte per voxel. Voxel positions and types are the app's signed
16-bit values, which the platform does not check: an edit the grid cannot hold (a type outside
0-255, a position outside 0-15) is kept whole in `ChunkData::overlay` (keyed by
`voxelKey(x, y, z)`), and `voxelTypeAt` / `voxelStateAt` return it. A game with other addressing
reads the raw edits through `onVoxel` and `StoredChunk::voxelStates`.

Buddy v0.37.0 echoes every accepted voxel edit back to its sender. `ChunkStore::setVoxel` records
each send (uuid, sequence, voxel) for 10 s and `ingest()` does not apply its echo again, so a local
edit fires `onChunkChanged` once; the echo is applied only when another client's edit of the voxel
arrived in between and no newer local edit is pending. `onVoxel` sees the echoes too. A state over
1,024 bytes (`wire::voxel::kMaxStateSize`) is `InvalidArgument` before anything changes; the server
answers it with `INVALID_REQUEST` (15). A paused app's sends are refused with
`wire::ErrorCode::AppPaused` (33).

## Voice payloads

An audio payload is opaque to the server, and a game with a voice format of its own keeps it.
`crowdy/media/voice_frames.hpp` is an optional one shared with CrowdyJS 18.7.0 (both SDKs replay
the same fixture, `tools/parity/fixtures/voice-frames.json`): a 10-byte header in front of each
codec frame, little-endian — the version (1), the codec (0 raw, 1 Opus 48 kHz mono, 2 G.711 µ-law
8 kHz), a `u16` seq, a `u32` timestamp in codec samples, the frame's duration in milliseconds and
two talk-spurt flags. `decodeVoicePacket` refuses a packet shorter than the header or of another
version. `VoicePacketizer` numbers one sender's frames; `VoiceJitterBuffer` puts each sender's
packets back in order, plays them 60 ms (`targetDelayMs`) after the first packet of a talk spurt
arrived, reports a frame that never came as a gap, drops one that arrives after its playout time,
and holds at most 64 frames a sender. It is single-threaded: push and pull from one thread.

```cpp
#include <crowdy/media/voice_frames.hpp>
using namespace crowdy::media;

VoicePacketizer packetizer({static_cast<std::uint8_t>(VoiceCodec::Opus), 20});
// Every 20 ms while the player talks (opusFrame from your encoder, or OpusVoiceEncoder):
const auto packet = packetizer.packetize(opusFrame, /*last=*/talkKeyReleased);
conn.sendAudio({chunk, session.actorUuid(), crowdy::Bytes(packet.data(), packet.size()), 1});
// ...and packetizer.skip() for every 20 ms of silence that is not sent.

VoiceJitterBuffer voices;
sessionConfig.onAudio = [&](const crowdy::replication::SpatialNotification& n) {
  voices.push(std::string(n.uuid, 32), n.payload, nowMs());
};
sessionConfig.onActorLeft = [&](const crowdy::core::ActorUuid& u, std::uint8_t) {
  voices.forget(std::string(u.data(), u.size()));
};
// At least once a frame:
for (const VoicePlayout& slot : voices.poll(nowMs())) {
  if (slot.gap) conceal(slot.key, slot.frameMs);  // the frame never came
  else play(slot.key, slot.codec, slot.frame);
}
```

### Channel audio (party and guild voice)

`sendAudio` reaches players near a chunk. `Connection::sendChannelAudio(channelId, uuid, payload)`
(opcode 35, Buddy v0.37.0) reaches every active member of a channel wherever they are, at most
1,024 payload bytes. The sender needs the channel's `send_voice` (`channels().create` with
`membersCanSpeak: true`, or `grids().createChannel(appId, gridId, name, true)`, gives it to the
member role) and the app's `use_voice_chat`; without them the server answers `UNAUTHORIZED` (7).
There is no echo. Members get opcode 36 through `Handlers::channelAudio` /
`WorldSessionConfig::onChannelAudio` (a `ChannelNotification`, like a channel message); key the
jitter buffer by channel and sender.

```cpp
const auto packet = party.packetize(opusFrame, talkKeyReleased);
conn.sendChannelAudio(channelId, session.actorUuid(), crowdy::Bytes(packet.data(), packet.size()));

sessionConfig.onChannelAudio = [&](const crowdy::replication::ChannelNotification& n) {
  partyVoices.push(std::to_string(n.channelId) + ":" + std::string(n.senderUuid, 32), n.payload, nowMs());
};
```

No codec is built by default. With `-DCROWDY_WITH_OPUS=ON` (see [Build](#build)),
`crowdy/media/opus.hpp` adds `OpusVoiceEncoder` (48 kHz mono 16-bit PCM in, one frame of 10, 20,
40 or 60 ms out) and `OpusVoiceDecoder` (`decode`, and `conceal` for a gap). Where a voice sits in
the world (panning, distance attenuation) stays the game's.

## Game Kit: social helpers and wire codecs

`makeKit(client, appId).social()` gives parties, guilds and chat rooms over the
platform's teams (membership and roles) and channels (messaging), with chat
sent over the native replication connection:

```cpp
auto kit = crowdy::kit::makeKit(game, appId, &connection);
auto party = kit.social().partyCreate("raid");
auto guild = kit.social().guildCreate("builders");
kit.social().chatSend(std::strtoll(guild.channelId.c_str(), nullptr, 10), "hello");
```

Game rules and state are ck-exec hubs (`client.exec()`). Two kit headers stand
alone: `crowdy/kit/wire.hpp` (the 48-byte engine pose codec, lanes, and the
parsers for server events 77 and 90-98) for games whose hubs keep that wire, and
`crowdy/kit/actions.hpp` (`runOptimisticAction`: apply locally, ask a referee
such as a hub endpoint, roll back on a denial). The blueprints, `deploy()`, the
engines and the model-backed kits went with the game model in 0.48.0.

## Wrapping CrowdyCPP in engines

CrowdyCPP is the intended foundation for engine-specific SDKs, including the
official [Crowdy Unreal SDK](https://github.com/CrowdedKingdoms/CrowdySDK-Unreal)
(docs: [Unreal SDK guide](https://docs.crowdedkingdoms.com/unreal-sdk/intro)).
The design rules that make it wrappable:

1. **No engine types, ever.** The public API uses `std::span`, `std::string_view`,
   and POD structs. Nothing in CrowdyCPP allocates with `new` on hot paths or
   leaks platform handles, so an engine can marshal at the boundary it chooses.
2. **Pluggable platform services.** Engines inject their own implementations:
   - `IHttpTransport` — Unreal wraps `FHttpModule` so all GraphQL traffic uses
     the engine's HTTP stack, proxies, and certificate handling. (Alternatively
     link the default libcurl transport; Unreal ships libcurl + OpenSSL in its
     ThirdParty tree.)
   - `IWebSocketTransport` / `IWebSocketConnection` — create a dormant socket,
     install its event callback in `start()`, and provide thread-safe,
     non-blocking `send()` / `close()`. The engine may complete on any thread;
     CrowdyCPP fences stale connections and posts user callbacks to `poll()`.
   - `ICrypto` — Unreal binds its bundled OpenSSL for HMAC-SHA256. Implement
     `makeHmacSha256()` as well: it returns a keyed `IMac` reused across
     datagrams, and it is the single largest CPU win on the replication path.
     Omitting it is supported and falls back to the one-shot call.
   - `ILogger` / `IAllocator` / `IClock` — adapters onto `UE_LOG`, `FMemory`,
     and engine time so SDK activity shows up in engine tooling.
3. **Threading stays with the engine.** Use manual-pump mode: the plugin runs
   `pump()` on an `FRunnable` network thread (or the task graph) and `poll()`
   on the game thread from a ticker. Callbacks therefore fire on the game
   thread, where `UObject`s are safe to touch. Nothing in CrowdyCPP spawns
   threads in this mode.
4. **Binary state stays binary.** Actor-state payloads are opaque bytes on the
   wire. An Unreal wrapper maps its entity replication snapshots directly into
   the payload span — no base64, no JSON, no intermediate copies. The
   open-source [cks-loadtest](https://github.com/CrowdedKingdoms/cks-loadtest)
   tool includes an Unreal-compatible 88-byte actor-state layout that
   interoperates with the current Unreal SDK's pose format.
5. **Session layer maps to entity systems.** `WorldSession`'s remote-actor
   registry (staleness, sample history) is exactly the input an engine wrapper
   needs to drive owner/proxy entity components and interpolation; the chunk
   cache backs voxel/terrain streaming; inboxes back chat and direct messages.

The expected Unreal integration shape: CrowdyCPP builds as a static library in
a `ThirdParty` module of the plugin; the plugin's subsystems (connection,
entities, voice, teams, persistence) become thin adapters over
`crowdy::CrowdyClient`, `crowdy::replication`, and `crowdy::session::WorldSession`,
replacing the plugin's bespoke networking while keeping its Blueprint-facing
API stable. Other engines (custom C++ engines, Godot via GDExtension, Unity
via a C shim) follow the same recipe.

## Two tokens, two clients

There is one API origin since 0.20.0, but still two tokens. CrowdyCPP follows the
platform's
[portal / app-scoped token model](https://docs.crowdedkingdoms.com/management-api/portals-and-app-tokens):

1. Sign-in (`login` / `registerUser`, magic link, or social/OIDC) yields an
   **identity session token** — account, studio
   admin and minting. Not accepted for gameplay.
2. Gameplay requires a short-lived **app-scoped token** per app
   (`portal().mintAppToken(appId)`), which is also the 64-octet HMAC key for
   native UDP. With an active native connection, rotate it through
   `refreshGameplayToken()` so the old socket is quiesced before the bearer
   changes and the same handlers reconnect under the fresh token. Use
   `portal().refresh()` directly only when no replication lifecycle needs to
   be preserved.
3. Build one identity client and one client per game. All world/UDP calls run
   on the game client. The identity client points at the shared entry origin;
   the game client points at the app's own datacenter (`gameApiUrl`) with
   `discoveryUrl` set so it can recover if that instance stops answering.

Persisting them keeps the same split. `FileTokenStore::sessionPath(dir, origin)`
names the session file, `FileTokenStore::appPath(dir, appId)` the gameplay one:

```cpp
crowdy::ClientConfig identityCfg;
identityCfg.httpUrl = apiOrigin;
identityCfg.tokenStore = std::make_shared<crowdy::graphql::FileTokenStore>(
    crowdy::graphql::FileTokenStore::sessionPath(stateDir, apiOrigin));
```

A session is **one per origin** and an app token is **one per app**, and the
naming is not a formality. Browser games keyed their credential by the game's own
path, so two games on one origin could not see each other's login: a player who
signed in for one was anonymous to the next and got bounced back to the portal.
What they stored was an app token anyway, which is per-game by definition, so
there was nothing to share even had the keys matched. Do not key a session by
anything per-game — that is the bug, not the fix.

## Versioning and binary compatibility

CrowdyCPP remains pre-1.0. Within a minor line, patch releases preserve public
source compatibility and ABI compatibility for the installed libraries.
Each new minor release may make source or ABI changes, even though the major
version remains `0`; consumers must review the migration notes and rebuild.

The installed CMake package follows that policy with `SameMinorVersion`.
For example, `find_package(CrowdyCPP 0.16 CONFIG REQUIRED)` can select a newer
`0.16.x` package, but it will not accept `0.17.x`. No compatibility is promised
between arbitrary `0.x` minors.

## Server compatibility

CrowdyCPP targets the current platform APIs and degrades gracefully on older
deployments:

- **`userAppState` round-trip:** older game-api builds stored the base64
  `state` input verbatim and re-encoded on read (reads returned
  base64(base64(bytes))); newer builds round-trip symmetrically. Decode
  defensively if you must read rows written through an old server.
- **ck-exec (`client.exec()`):** a Game API that does not serve the `exec*`
  roots rejects the call with a GraphQL validation error.

## Errors

GraphQL-layer failures throw structured exceptions mirroring CrowdyJS:
`CrowdyHttpError`, `CrowdyGraphQLError` (preserves `extensions.code`,
`remediation`), `CrowdyNetworkError`, `CrowdyTimeoutError`,
`CrowdyProtocolError`. Branch on `error.code()` rather than parsing messages.
`graphql::accessRefusalOf(errors)` (ACCESS_REVOKED, ACCESS_SUSPENDED with `suspendedUntil`,
ACCESS_NOT_GRANTED), `appPausedOf` (APP_PAUSED, `reason`) and `actorExistsOf` (ACTOR_EXISTS,
`ownedByCaller`) read the refusals a player should be told about from `CrowdyGraphQLError::errors()`
or `GraphQLOutcome::errors`. A paused app still mints: check
`domains::isAppPaused(token.runtimeGate)` before entering the world.
Subscriptions are non-throwing: `onNext` receives
`GraphQLSubscriptionOutcome`, while `onError` receives a typed terminal
`GraphQLSubscriptionError`. Destroying its move-only handle suppresses queued
callbacks and sends protocol `complete` when connected.

The replication layer never throws on the hot path: sends return
`crowdy::Result` codes, server-reported failures arrive as
`GenericError` notifications correlated by sequence number, and connection
state changes surface through a status callback. Note the protocol's
documented semantics: UDP is best-effort, sequence numbers are correlation
(not idempotency), and **auth failures are often silent drops** — see
[Operations](https://docs.crowdedkingdoms.com/replication-api/operations).

## Schema refresh and codegen

The GraphQL surface is generated from committed artifacts so external builds
never need network access or sibling repos:

```bash
# Maintainer-only Node dependencies (not part of a CMake consumer build).
npm ci

# Maintainers: refresh the unified published SDL
node scripts/schema-sync.mjs            # writes schema.gql from game-api.graphql
node scripts/codegen.mjs                # regenerates include/crowdy/generated/
# commit schema.gql and include/crowdy/generated/ together
```

`scripts/schema-sync.mjs` downloads the published unified SDL
(`https://docs.crowdedkingdoms.com/schema/game-api.graphql`); `--game <path|url>`
overrides the source. There is one schema because there is one origin: the
published management SDL is a **derived** subset for the docs tab, not a second
endpoint. A sync also records where the snapshot came from, in
`package.json`'s `publishedSchemaSnapshot` — commit that alongside `schema.gql`.

`schema.gql` is the **pinned artifact**: it is what every external build compiles
against, and CI checks it **offline**
(`node scripts/schema-sync.mjs --check --offline`) — that it is in canonical form
and matches its provenance record, both of which are properties of the commit.
CI used to compare it against the live SDL, which meant an unrelated commit could
turn red hours after it was pushed because someone republished the docs site.
Whether the snapshot has fallen *behind* the published SDL is a real question and
is asked daily by `.github/workflows/schema-drift.yml`, which files an issue
naming the `cks-docs` commit that published the change, which fields moved, and
what to run. `--check` without `--offline` performs that live comparison by hand. Operation documents live in `operations/<domain>/*.graphql` and follow
the same shapes as CrowdyJS. Codegen isolates each named operation with only
its transitive fragments and embeds schema and operation-input digests in
generated headers; `node scripts/codegen.mjs --check` verifies them without
modifying files.

### Parity maintenance gates

CrowdyCPP tracks CrowdyJS **18.0.0**. The source of truth is
`crowdyjsParityTarget` in `package.json` — quote it from there, not from this
sentence, which said 14.1.0 at a commit hash for a day after 0.26.0 moved the pin; CI reads that commit before checkout,
and the parity/fixture tools reject a checkout whose package version or HEAD
does not match. After either SDK changes its public surface:

```bash
# Optional for nonstandard layouts. Otherwise tools resolve ../CrowdyJS,
# ./CrowdyJS (CI), then the sibling of this worktree's primary git checkout.
export CROWDYJS_PATH=/path/to/CrowdyJS

# Compare both schemas in both directions, audit roots/methods, and refresh docs.
node tools/parity/parity.mjs --crowdyjs "$CROWDYJS_PATH" \
  --write docs/parity-matrix.md --strict
npm run check:operations
npm run check:parity

# CrowdyJS must be built first. Every fixture tool rejects tracked checkout
# changes, a wrong package version, or a wrong commit.
npm run check:layout-fixtures
npm run check:studio-state-fixtures

# Parser/gate behavior.
npm test
```

An explicitly configured `CROWDYJS_PATH` is authoritative and fails clearly
when invalid. Automatic resolution likewise fails if no deterministic
sibling/CI/worktree checkout exists; it never skips or weakens parity.

The reviewed baseline accepts only named classifications. A **portable gap** is
shown as missing work and is not presented as parity; native equivalents and
inherently browser-only surfaces are the only waivers. New differences and
stale classifications fail. `--strict` additionally fails on every remaining
portable gap and is the strict portable-parity release gate used by CI.

When intentionally changing the target, update the pinned CrowdyJS SHA, sync
the descriptor/preemption, control-gate, 11-tool Studio host, and layout
fixtures with their `tools/parity/*-fixtures.mjs --write` commands, validate
the shared Studio state fixtures, regenerate `docs/parity-matrix.md`, and
commit the pin plus changed fixtures and generated evidence together. Include
schema snapshots and generated headers whenever the target also changes SDL.
None of these maintainer gates run during a normal external CMake build.

## Tests

- `ctest` — offline unit tests (wire codec golden vectors, HMAC vectors,
  GraphQL-WebSocket handshake/reconnect/frame/cancellation behavior, bundle
  parsing, malformed-input fuzz, codec round-trips, and the CrowdyJS cases the
  native code replays: the exec gateway and voice payload fixtures). A build
  configured with `CROWDY_WITH_OPUS=ON` adds `opus_test`.
- A build configured with `CROWDY_NO_EXCEPTIONS=ON` compiles with
  `-fno-exceptions` and runs the applicable reduced-surface matrix. The
  package omits exception-contract layers listed in [Build](#build), and its
  install test verifies those unsupported headers are not shipped.
- `npm test` — offline Node tests for schema/parity parser behavior.
- `tests/e2e/` — end-to-end suites (two-client fan-out, gamer journey, token
  refresh/reconnect, opt-in marketplace chunk claim/release, and the complete
  native Studio factory/edit/BUILD/draft/Play takeover lifecycle) that run
  against a deployment you configure via
  environment variables (`CROWDY_E2E_API_URL`, `CROWDY_E2E_HTTP_URL`,
  `CROWDY_E2E_EMAIL`, `CROWDY_E2E_APP_ID`, …). Skipped when unset.
- `tests/prodsmoke/` — a read-only smoke test of the discovery and endpoint-move
  path against a live tier. Not part of any build and not run by CI; it needs the
  network. It exists because the estate rule is the one piece that cannot be
  proven by a fixture: a guard that refused
  `ck.<tier>.crowdedkingdoms.com -> ck-<dc>.<tier>.crowdedkingdoms.com` would pass every
  test in this repo and then decline every redirect in production. Build it
  against an installed package and run:

  ```bash
  cmake -S tests/prodsmoke -B build-prodsmoke -DCMAKE_PREFIX_PATH=<install-prefix>
  cmake --build build-prodsmoke
  ./build-prodsmoke/prod_smoke https://ck.prod.crowdedkingdoms.com <appId>
  ```
- `benchmarks/` — codec ns/op, the send-path cost breakdown (`bench_send`:
  encode vs MAC vs socket write, with the alternatives for each priced side by
  side), and end-to-end echo latency against an env-configured deployment. See
  [benchmarks/README.md](benchmarks/README.md) for how to run them and the
  recorded results.

## Docs

- [Replication API (native UDP)](https://docs.crowdedkingdoms.com/replication-api/intro)
- [Wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats) · [HMAC](https://docs.crowdedkingdoms.com/replication-api/hmac)
- [Management API](https://docs.crowdedkingdoms.com/management-api/intro) · [Game API](https://docs.crowdedkingdoms.com/game-api/intro)
- [Agentic Studio from a native client](docs/native-agent-api.md)
- [Native Studio integration](docs/native-studio-integration.md) · [Native player host](docs/native-player-host.md) · [GraphQL WebSockets](docs/graphql-websocket.md)
- [CrowdyJS / CrowdyCPP / Game API compatibility](docs/compatibility.md)
- [Release verification checklist](docs/release-checklist.md)
- [Grids & permissions](https://docs.crowdedkingdoms.com/game-api/grids-and-permissions)
- [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) — the TypeScript SDK this API surface mirrors
- Agent index: [llms.txt](https://docs.crowdedkingdoms.com/llms.txt)

## License

MIT
