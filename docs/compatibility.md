# SDK and Game API compatibility

CrowdyCPP 0.74.0 passes the strict portable-parity gate against CrowdyJS
**18.22.0**. The gate pins CrowdyJS commit
`6d413fc125c73d5467c1e66fa601ec8c8f98fbf2` (`dev/v18.22.0`; `crowdyjsParityTarget` in
`package.json`); see [`parity-matrix.md`](parity-matrix.md) for the generated
method-by-method evidence. Native equivalents and browser exclusions remain
intentional, so this does not claim identical transports or browser behavior.

0.74.0 mirrors CrowdyJS 18.22.0 (ck-api v2.58.0, ck-exec 0.19): `exec().instances` selects `orphaned`, true for an instance whose node type the app's active version no longer deploys (a deploy or a rollback dropped the type, stopped the instance and kept its saved state), null from a manager older than ck-exec 0.19. It needs ck-api v2.58.0 on the tier, which an older API refuses to select. The schema's deploy, rollback, switch and restart descriptions say what a deploy or a rollback now stops and unswitches. No method changed.

0.73.0 mirrors CrowdyJS 18.21.0, a send waiting across a new session: an actor or voxel update still waiting when a reassignment's session is connected (`sessionEpoch()` moved) is resent once then, with a fresh sequence, instead of at its timeout (`Connection::awaitSequence`, `WaitOutcome::sessionChanged`); text is never resent. CrowdyJS ends the wait on the realtime status turning `connected` with a new epoch.

0.72.0 mirrors CrowdyJS 18.20.0 (realtime server v0.42.0), a session's first awaited send: every `*AndWait` retries the first awaited send of a session once, with a fresh sequence, when it is refused `Unauthorized` or, for the echoed actor and voxel updates, when the echo times out; text's clean timeout is its success and is never retried. `Connection::sessionEpoch()` counts the sessions (each assignment). CrowdyJS also moved its echo window to start at acceptance (`RealtimeClient.expectSequence`); CrowdyCPP's wait already did, its deadline set after the send has left.

0.71.0 mirrors CrowdyJS 18.19.0 (ck-api v2.56.0), the draining move: a draining instance expires the balancer's affinity cookie (`cks_ga`) and names a peer, and the client moves its HTTP origin and its subscription together. CrowdyCPP already did both on `SERVER_DRAINING` (`moveToDatacenter`), and libcurl's shared cookie jar drops the expired cookie; `lb_affinity_cookie_test` pins that. No method changed.

0.70.0 mirrors CrowdyJS 18.18.0 (ck-api v2.55.0): the schema's `replayLoggingRecordsMedia` and `applyCommunicationDefaults`, and the send comments that name `use_text_chat` / `use_direct_messages`. No method changed. The operator's account-erasure and legal-hold fields (`accountErasure`, `requestAccountErasure`, `legalHolds`, `setLegalHold`, `releaseLegalHold`) are browser exclusions in both SDKs, like every operator-only root.

0.69.0 mirrors CrowdyJS 18.17.0, the Org-Managed Players Addendum (ck-api v2.53.0, org-managed players on every tier): the `orgLegalAcceptanceRequiredOf` reader (`ORG_LEGAL_ACCEPTANCE_REQUIRED`: the organization has not accepted, so it signs no player in and their tokens do not refresh) and the terms fields on `managedPlayers().settings()`. `acceptManagedPlayerTerms` is a browser exclusion in both SDKs: an admin of the organization accepts in Studio.

0.68.0 mirrors CrowdyJS 18.16.0, org-managed players: `portal().adoptAppToken` and `exchangeManagedPlayerAssertion`, `admin().managedPlayers()`, `revokeToken(id, endSessions)`, and the `assertionRejectedOf` / `managedPlayerNotAllowedOf` readers. 0.67.4 adds `ChunkStore::setVoxel(..., optimistic)` (CrowdyJS's `optimistic: false`: applied when the server's echo arrives) and re-pins to CrowdyJS 18.15.0, whose `udp.sendGenericSpatial` sends opcode 140 on the binary relay as `Connection::sendGenericSpatial` does; CI builds the Opus wrapper. 0.67.3 re-pins to CrowdyJS 18.14.0: the schema's billing descriptions describe the lossless ledger, and `playerWallet().charges()` is deprecated with the `playerUsageCharges` field it wraps (read `transactions()`). 0.67.2 drops a kit's subscriptions and connection listeners on `close`, and runs each queued callback once; the parity pin is unchanged. 0.67.0 sends both fields of `updateGamertag` (the gamertag and its disambiguation). 0.66.0 mirrors CrowdyJS 18.13.0, the economy's kit modules. 0.65.0 mirrored 18.12.0, the
genre starters' kit modules. 0.64.0 mirrored 18.11.0,
the Game Kit on ck-exec. The gate reads CrowdyJS's
`src/kit/modules/` as well since then, so the typed clients for the starter hubs are
held method by method like every other class.

Since 0.31.0, async app-token refresh can name the current replication server:
`portal().refreshAsync(ip4, port)` sends `refreshAppToken(currentServer)` (the
same document as the blocking overload). `CrowdyClient::refreshGameplayTokenAsync`
uses that form when the quiesced connection still has an assignment. An empty
`authorizedServer` on a successful outcome is not a failed `GraphQLOutcome`.

**The pinned commit must be one this tier has actually been promoted.** Before
2026-09-02 all three branches pinned `8937075…`, a merge commit created on
CrowdyJS `prod` and therefore reachable from `prod` and from neither `dev` nor
`test` — so the `dev` and `test` lines were claiming parity against another
tier's commit. `check-parity-target-tier.mjs` refuses that now.

The rule is **reachability from this tier's branch**, which is the same thing as
"this commit has been promoted to me". Promote CrowdyJS to a tier BEFORE
promoting this pin there.

**And it must be this tier's published release** (checked since 2026-10-10): the
commit is the target of CrowdyJS's `<tier>/v<version>` tag for the pinned version,
where the tier is this branch's (a feature branch counts as `dev`, a
`promote/<from>-to-<to>-…` branch as `<to>`; a pull request is judged by its base),
checked in CI by `tools/parity/pin-tag.mjs` on pull requests and on pushes to `dev`,
`test` and `prod` (a pushed promotion branch used to be judged as `dev` and fail). Reachability
says the commit was promoted, not that it was released: 0.59.0 pinned `ca9fbb5` as
18.6.0, reachable from `dev`, while `dev/v18.6.0` is `7662d0b0` (`ca9fbb5` first
shipped in `dev/v18.7.0`). CrowdyJS tags each tier's release at the commit that
tier released, so one version is three commits (`dev/v18.4.0` `4e7e595b`,
`test/v18.4.0` `2bfbb081`, `prod/v18.4.0` `7f805609`), and **this line differs per
tier**: each branch pins its own tier's tag. A promotion re-pins to the
destination's tag once CrowdyJS has released the version there; the check refuses
a `test` pin of a release CrowdyJS has made only on `dev`, and a `dev` pin of a
`prod/v…` commit. `promote.mjs` pins CrowdyJS at `origin/<to>`, which is the tag's
commit when nothing has landed on that branch since CrowdyJS's release; when it is
not, the check names the commit to pin.

Reachability's asymmetry is deliberate and is not a gap. `prod` may reach a commit
that also lives on `dev`, because reaching `prod` means it was promoted there;
`dev` may not pin a `prod`-only merge commit, because that commit has never been
promoted back. The tag rule then narrows each tier to its own release's commit.
Pin a **commit**, never a branch head — an ancestor keeps the generated
fixtures reproducible, whereas a moving head is the "same-version moving branch"
the pin exists to prevent. Moving the version is a separate, deliberate act; see
[`release-checklist.md`](release-checklist.md).

| Surface | CrowdyCPP 0.74.0 | CrowdyJS 18.22.0 | Required public API generation |
|---|---|---|---|
| Core Management and Game GraphQL | Supported for players, developers and org-admins; nothing only a super-admin or an operator can call (0.51.0) | The same audience (18.0.1) | Current published Management + Game SDL |
| Org-managed players | `portal().adoptAppToken` (refuses a `cko_` org token) and `exchangeManagedPlayerAssertion`; `admin().managedPlayers()` (`list`, `get`, `provision`, `update`, `endSessions`, `remove`, `settings`, `signingKeys`, `registerSigningKey`, `revokeSigningKey`, `appModsEnabled`, `setAppMods`); `admin().organizations().revokeToken(id, endSessions)`; `graphql::assertionRejectedOf`, `managedPlayerNotAllowedOf` (0.68.0); `graphql::orgLegalAcceptanceRequiredOf` and the addendum fields on `settings` (0.69.0). No org-token mint and no addendum acceptance | The same, with `client.managedPlayers`, `organizations.revokeToken(id, { endSessions })` (18.16.0) and `orgLegalAcceptanceRequiredOf` (18.17.0) | ck-api `v2.52.0` (dev); dev, test and prod since `v2.53.0`, after the organization's admin accepts the Org-Managed Players Addendum in Studio |
| ck-exec | `exec().connect` / `connectAsDeveloper` / `ExecConnection` over an injected or curl WebSocket, MessagePack via `graphql::Json::toMsgpack` / `fromMsgpack`; `logs` (with the `flow` filter), `instances`, `versions` (with `manifestJson`), `endpointStats`, `status`, `activateVersion`, `setEnabled`; `ExecReply::rateLimited` / `retryAfterMs`; `starters`, `build`, `buildStatus`, `waitForBuild`, `deploy` with a build id; mods (`modStarter`, `modBuild`, `waitForModBuild`, `modDeploy`, `modSetEnabled`, `modDelete`, `mods`, `myMods`, `modLogs`, the marketplace, `appMods`, `modSwitches`, `modSetSwitch`) and `execModType` | `exec.connect` / `connectAsDeveloper` / `ExecConnection`, `@msgpack/msgpack`; the same operations (`versions` also parses `manifest`), builds and mods; `CrowdyExecError.rateLimited` / `retryAfterMs` | Game API dev `execConnect` / `execConnectAsDeveloper` / `execDeploy`, the operations, `execBuild` / `execBuildStatus` / `execStarters` and `execMod*` (ck-api `v2.20.0`), `execEndpointStats`, `execLogs(flow)` and `ExecVersion.manifestJson` (ck-api `v2.22.0`), `ExecInstance.orphaned` (ck-api `v2.58.0`); ck-exec v0.2 client protocol. Builds and listings select `ExecBuild.kind` and the CLIENT fields, so they need ck-api `v2.24.0` |
| ck-exec CLIENT halves | `exec().modClientBuild`, `modClientDeploy`, `modClientDelete`, `gridClientMods`, `consentClientMod`, `trustAuthor`, `revokeClientModConsent`, `revokeAuthorTrust` (0.52.0), `modClientArtifact`; `modClientArtifactBytes` refuses bytes that differ from their SHA-256 digest, a CLIENT ABI other than `kExecClientAbiVersion` (0) and a capability summary that does not parse (`parseExecClientCapabilitySummary`). No WASM runtime: the engine runs the module in its own sandbox | The same operations; `ExecClientHalves` runs a grid's CLIENT halves in `PlayerCodeBroker` (`engine: 'ck-exec'`) in the page's glue worker, and `revoke` / `forgetAuthor` take them back | Game API dev `execModClientBuild` / `execModClientDeploy` / `execModClientDelete` / `execGridClientMods` / `execConsentClientMod` / `execTrustAuthor` / `execModClientArtifact` (ck-api `v2.24.0`); `execRevokeClientModConsent` / `execRevokeAuthorTrust` (ck-api `dev/v2.28.0`). The legacy engines' fields are gone from ck-api `v2.27.0`, and so is this SDK's surface for them |
| ck-exec connect token and gateway | `exec().connect` / `connectAsDeveloper` dial only a gateway `execGatewayRefusal` passes (`wss:` under an `https:` Game API, on the Game API's or the default origin's estate, loopback for a loopback Game API); a gateway's `HTTP 401` is `Denied` with its reason (`WebSocketError::httpStatus` / `httpBody`; the curl transport reports the status alone), `429` is `Unavailable`, 4401 stays `Denied`; `ExecConnection::lastFailure()` says why an attempt failed (0.55.0) | The same check (`execGatewayRefusal`, the cases in `exec-gateway-cases.json`); under Node's `ws` a `401` is `Denied` with its body, in a browser `Unavailable` (18.1.0) | ck-exec 0.10.0+ answers a refused upgrade `401` / `429` before any WebSocket |
| Open grids | `gameApps().openPermissions` / `setOpenPermissions` (also `admin().grids()`, 0.55.0) | `gameApps.openPermissions` / `setOpenPermissions` (18.1.0) | `gridOpenPermissions` / `setGridOpenPermissions` (cks-game-api #436, ck-api `dev/v2.31.0`, `manage_apps`) |
| Terms and age gate | `auth().recordPlayerConsents` / `recordPlayerConsentsAsync`, `auth().playerLegalAcceptance` / `playerLegalAcceptanceAsync`, the clickwrap overload of `auth().registerUser(email, password, gamertag, acceptLegal, attestAgeOfMajority)` (0.57.0) | `auth.recordPlayerConsents`, `auth.playerLegalAcceptance`, `auth.register`'s two fields, `isLegalAcceptanceRequiredError` (18.4.0) | `recordPlayerConsents` / `playerLegalAcceptance` (ck-api v2.35.0); a gameplay token answers `LEGAL_ACCEPTANCE_REQUIRED` until both are stored |
| The input log | `inputLog().sessions` / `messages` (with `Async` twins), `App.replayLoggingEnabled` on every app read (0.59.0); a message's `body` is standard base64, decoded with `crowdy::core::base64Decode`. Since 0.62.0: `missingRecords` on every session; `graphql::inputLogReadRetryable` and `inputLogFundsNeededOf` (`requiredMicrousd` / `spendableMicrousd`); a `graphql::CancelToken` overload of each read, beside the timeout | `inputLog.sessions` / `messages`, `App.replayLoggingEnabled` (18.6.0), `decodeBase64`; `missingRecords`, the retryable codes, `signal` on both reads, a request's `signal` kept beside its timeout (18.9.0) | **ck-api v2.39.0 or later**: every app read selects `replayLoggingEnabled`, which an older Game API refuses. Since 0.62.0 the sessions read selects `missingRecords`, so it needs **the ck-api release after v2.40.2**, which also brings `INPUT_LOG_TEMPORARILY_UNAVAILABLE` / `INPUT_LOG_RATE_LIMITED` and the `logging_off`, `shutdown`, `paused` and `unrecorded` end reasons. Turning logging on (`updateApp`) answers `INPUT_LOG_FUNDS_NEEDED` without a spendable wallet or a billing exemption |
| Native UDP replication | Direct native transport | Browser GraphQL UDP proxy | Current Replication API |
| Distance-limited channel messages | `Connection::sendRangedChannelMessage(channelId, uuid, payload, origin, maxDistance)`, `wire::encodeRangedChannelMessage` (0.58.0) | `udp.sendRangedChannelMessage`, `serializeRangedChannelMessage` (18.5.0) | Buddy v0.35.0 (opcode 32); ck-api `sendRangedChannelMessage` for the browser proxy; members receive the ordinary channel notification |
| Webcam video + actor-left | `Connection::sendVideo` / `sendVideoFrame`, `Handlers::video` / `actorLeft`, `media::VideoFrameAssembler`, `RemoteActorStore::remove` | `udp.sendVideoPacket` / `sendVideoFrame`, `video` / `actorLeft` handlers, `VideoFrameAssembler`, store remove-on-leave | Buddy v0.25.0 (opcodes 143/144/145), Game API v1.87.1 (`use_video_chat`) |
| Voice payload helpers | `crowdy/media/voice_frames.hpp`: the 10-byte voice header, `VoicePacketizer`, `VoiceJitterBuffer`, replaying `voice-frames.json`; optional libopus wrapper `crowdy/media/opus.hpp` with `CROWDY_WITH_OPUS=ON` (0.60.0) | `media/voice-frames.ts`, the fixture's owner; Opus through the browser's WebCodecs (18.7.0) | None: the payload of opcodes 134/135, which the server never reads |
| App-defined spatial messages (opcode 140) | `Connection::sendGenericSpatial`, `Handlers::genericSpatial`, `WorldSessionConfig::onGenericSpatial` (0.60.0) | `genericSpatial` handler and World Stores bus key (18.7.0) and `udp.sendGenericSpatial` (18.15.0), binary relay only | Buddy opcode 140; the GraphQL `udpNotifications` union has no member for it |
| Channel audio | `Connection::sendChannelAudio`, `Handlers::channelAudio`, `WorldSessionConfig::onChannelAudio`, `wire::encodeChannelAudio`; `grids().createChannel(..., membersCanSpeak)` (0.60.0) | `udp.sendChannelAudio`, `channelAudio` handler and bus key (on the binary relay and the GraphQL transport), `serializeChannelAudio`, `membersCanSpeak` (18.7.0) | Buddy v0.37.0 (opcodes 35/36); the channel's `send_voice` and the app's `use_voice_chat`; ck-api `sendChannelAudio` and the `udpNotifications` union's `ChannelAudioNotification` for the browser proxy; CrowdyCPP's control-only `udpNotifications` document does not select it, since the native client receives 36 itself |
| Voxel edits for a game's own world | `WorldSessionConfig::onVoxel`, after `chunks()` merged the edit (0.60.0) | `voxelUpdate` handler and bus key | None |
| The echo of a client's own voxel edit | `ChunkStore` skips the echo of its own `setVoxel` unless a foreign edit came in between; state over 1,024 bytes is `InvalidArgument` (0.60.0); `setVoxel(..., optimistic = false)` applies only the echo (0.67.4) | `ChunkStore` does the same, `optimistic: false` included; `assertVoxelEdit` (18.7.0) | Buddy v0.37.0 echoes every accepted edit to its sender and refuses a state over 1,024 bytes with `INVALID_REQUEST` (15) |
| Paused apps and access refusals | `AppTokenResponse::runtimeGate`, `domains::isAppPaused`, `wire::ErrorCode::AppPaused`, `graphql::appPausedOf` / `accessRefusalOf` / `actorExistsOf`; `admin().appAccess().suspend` / `unsuspend` / `resyncTierGridPermissions`; `users().playerProfile(s)`; `exec().restartType` (0.60.0) | `runtimeGate`, `isAppPaused`, `UDP_ERROR_NAMES[33]`, the same readers and wraps (18.7.0) | The ck-api release after v2.39.0 (`runtimeGate` is selected on every token mutation, so an older one refuses the mint); Buddy v0.37.0 for error 33 |
| Bans, account blocks and the access-ended close | `channels().ban` / `unban` and `teams().ban` / `unban` (with `Async` twins); `graphql::accountBlockedOf` (`blockedUntil`) and `groupMemberBannedOf`; `ExecConnection` ends for good on `kExecCloseAccessEnded` (4403): no reconnect, calls in flight `Denied`, `onAccessEnded` hears the reason (0.63.0) | `channels.ban` / `unban`, `teams.ban` / `unban`, `accountBlockedOf` / `groupMemberBannedOf`, `EXEC_CLOSE_ACCESS_ENDED` and `ExecConnection.onAccessEnded` (18.10.0) | **The ck-api release after v2.43.1** (cks-game-api #514) for the four ban mutations; nothing else selects a new field. ck-exec 0.16 for the 4403 close, which an older host never sends. `blockAccount` / `unblockAccount` / `accountBlock` are super-admin and wrapped by neither SDK |
| The Game Kit on ck-exec | `makeKit(client, appId, &connection)`: `world()`, `matchmaking().queue(name)`, `sessions().open(key)`, `npcs().area(key)` / `areaAt`, `topology()` (blocking), `spectate()`, `emotes()`, `shards()`, `runtime()` (`KitRuntime`: the call budget, safe resends, the 256-subscription cap, `KitReply::code()`), `HubState` / `CollectionMirror` / `PrivateState`, `decodeKitActorState`, events 99-101; `ExecConnection::call(..., timeoutMs)`, jittered reconnects, public `pinGateway` (0.64.0) | `client.kit(appId)`: the same modules, `KitRuntime`, `KitError`, the mirrors, `kitActorCodec`, `execWireIntegers` (18.11.0) | A compute toolchain with ckx-sdk 0.10.0 and the starters that speak the kit protocol (dev v1.12.0, 2026-10-15) and the starters deployed in the app (`topology().deploy`); nothing new in the Game API |
| The genre starters | `leaderboards()`, `tables()`, `tactics()`, `encounters()`, `minigames()` (`throwHand`), `instances()`, `arenas()`, `racing()` (`race`, `ball`), `territory()` (`map`, `zone`) in `crowdy/kit/genres.hpp`; `GameKitNodeTypes` names their types (0.65.0) | `kit.leaderboards`, `tables`, `tactics`, `encounters`, `minigames`, `instances`, `arenas`, `racing`, `territory` (18.12.0) | A compute toolchain with ckx-sdk 0.11.0 and the genre starters, deployed in the app; nothing new in the Game API |
| The economy | `player()` and `inventory()`, `wallet()`, `progression()`, `achievements()`, `quests()`, `crafting()`, `loot()`, `production()`; `shops()`, `market()`, `trades()` `.at(key)` in `crowdy/kit/economy.hpp`; `codes::kUnavailable` resent (0.66.0) | `kit.player` and the same views, `kit.shops` / `market` / `trades` `.at(key)`, `KIT_CODES.UNAVAILABLE` (18.13.0) | A compute toolchain with ckx-sdk 0.12.1 (dev: v1.15.0) and the `player` and `market` starters, deployed in the app; nothing new in the Game API |
| The SDK a build compiled against | `sdkVersion` in the JSON of `exec().build`, `buildStatus`, `waitForBuild`, `modBuild`, `modClientBuild`, `modBuildStatus` (0.61.0) | `ExecBuild.sdkVersion` through the same calls (18.8.0) | ck-api v2.40.2 (`ExecBuild.sdkVersion`; an older API refuses the build documents) |
| Wide voxel types and other addresses | `ChunkData::overlay` (`OverlayVoxel`, `voxelKey`); `voxelTypeAt` returns `std::int16_t` (0.60.0) | `CachedChunk.overlay` (`ChunkOverlayVoxel`, `voxelKey`); `createGridHostCalls({ voxelBounds })` for a CLIENT half (18.7.0) | Voxel positions and types are the app's signed 16-bit values |
| Bundled uplink sends | `Config::bundleSends` / `bundleWindowMs`, `Connection::flushSends`, `Stats::bundlesSent` / `messagesDropped` | `realtime.bundleSends` / `bundleWindowMs`, `udp.flushSends`, `realtime.binaryRelayStats` (binary relay only) | Buddy v0.27.0 (client `MESSAGE_BUNDLE`) |
| Wrapper seams | `Config::onEventsReady` (wake an event loop when notifications are waiting), `IChunkSource` for `ChunkStore` and `IHostElection` + `WorldSessionServices` for `WorldSession` (durable services without a `CrowdyClient`), `WorldSessionConfig::onText` (0.54.0) | No counterpart: the browser SDK has no native core to wrap | None; native-only API |
| Recorded voxel edits on chunk loads | `ChunkStore::ensureAround`'s one `getChunksByDistance` selects `voxelStates` and puts each entry over the stored grid (type at its voxel, its state; an entry without one clears the cached state); `IChunkSource` reports them in `StoredChunk::voxelStates` (0.56.0) | `ChunkStore.hydrate` reads `getChunk`'s `voxelStates` per loaded chunk, which `ensureAround` runs only with a `voxelStateCodec` or `hydrateVoxelStates: true` | ck-api `dev/v2.33.0` (cks-game-api #445): both chunk reads return every recorded edit (a hub's or mod's `world.set_voxels`, `updateVoxel`, realtime voxel updates) as an entry, `getChunksByDistance` only when `voxelStates` is selected; a chunk with edits but no stored row is returned by neither |
| Chunk write-back failures | `ChunkStore` sends a refused write-back once and drops it, retries one that can clear up to 5 attempts (waits 0.7/1.4/2.8/5.6 s) and drops it, and reports both through `onWriteBackFailed`; `flush()` returns a `ChunkFlushResult` (0.53.0) | `ChunkStore` does the same; `onWriteBackFailed`, and `flush()` returns the failures (18.0.4) | `extensions.code` / `retryable` / `httpStatus` on Game API errors; a closed wilderness (`App.wildernessWritesOpen`, ck-api `dev/v2.30.0`) refuses with FORBIDDEN |
| Generic GraphQL WebSocket | `GraphQLSubscriptionClient` | `graphql-ws` | `graphql-transport-ws` endpoint |
| App listing-version administration | `marketplace().appListingVersions` | `marketplace.appListings` (no listing-version method) | Management API 2026-07-24+ |
| Tier features | `admin().appAccess().defineFeature` / `features` / `grantTierFeature` / `revokeTierFeature` / `tierFeatures` | `appAccess.defineFeature` / `features` / `grantTierFeature` / `revokeTierFeature` / `tierFeatures` | `gameModelDefineFeature` … `gameModelTierFeatures`, served beside access tiers |
| Crowdy Studio projects/runtime | Headless native controller, typed diagnostics/wallet observation; a GITHUB project's `saveProject` commits each changed file; the SERVER target is a ck-exec mod and the CLIENT target its CLIENT half (`CrowdyStudioModRuntime`), run by an engine-owned client runtime | Browser/headless controller; bound saves commit through `crowdyStudioGitHubPutFile` / `DeleteFile`; the SERVER target is a mod and the CLIENT target its CLIENT half, previewed in the page's broker | Game API project roots, `execMod*` and `execModClient*` (Studio previews need ck-api `v2.25.1`); durable checkpoint mutations require an injected bridge |
| Crowdy Studio pane layout | Headless controller with injected storage | Headless controller with browser-local default storage | None |
| Native Studio integration | Owned editor/layout/runtime assembly with explicit maintenance scheduling | Browser Studio composition with the in-browser DSH agent pane | Project/runtime roots |
| Agentic Studio policy, consent, metered usage | `CrowdyStudioAgentAPI` reads and org-admin writes (the operator platform policy, app kill and catalog are not wrapped) | `CrowdyStudioDshTransport` reads; the harness spends through REST `/v1/model` | Game API with the metered model endpoint (removes the 21 `crowdyStudioAgent*` session/run/lease/tool roots) |
| Player-host observation | Typed `PlayerHostAdapterV1` + schemas (observe only) | `PlayerHostAdapterV1` observe for `game_observe` | `crowdy.player-host/1` |

`schema.gql` is the committed snapshot of the published API SDL. Codegen
isolates every operation with only its transitive fragments and validates it
independently, so an unrelated root field in the same file cannot make a
request valid.

Since 0.20.0 there is one schema because there is one origin. The per-plane
snapshots are gone: the published management SDL is now derived from the unified
schema by filtering it to a root-field allowlist, so validating against it would
have answered "is this in the management docs tab" rather than "will the server
accept it".

Older servers reject only operations they do not know. A client can continue
using older surfaces by not calling the newer methods. There is no provider
key or provider client in CrowdyCPP: Agentic Studio provider selection and
credentials remain server-side.

The only intentional parity waivers are generated in the parity matrix:
native equivalents for browser UDP/runtime behavior and browser exclusions
for inherently browser-owned PKCE persistence, DOM/Monaco/VFS worker chrome,
splitters, embed panel/dock/HUD/styles/focus handling, worker-entry
packaging, and the player-WASM runner, broker and glue that run CLIENT
modules in a Web Worker (`ExecClientHalves`, `PlayerCodeBroker`,
`GlueRuntime`, the host-call allowlist, and 18.0.0's `invoke` / `onLog` on
the runner and grid host-call answers). CLIENT artifact-byte decoding is
portable: `exec().modClientArtifactBytes(...)` fetches a mod's CLIENT half and
checks the bytes against their digest. World Stores behaviour that changes
without a method (17.14.0's actor resend, autosave retry and bounded chunk
hydration; 18.7.0's chunk overlay, voxel hook and opcode 140, and the CLIENT
half's `voxel_set` bounds) is pinned in the matrix's behavior audit, and the
voice helpers' every export is mapped to `crowdy/media/voice_frames.hpp`.
Portable gaps, unclassified differences, and stale classifications are zero.

## CrowdyCPP 0.x source and ABI policy

Until 1.0, each minor release may contain source-incompatible or ABI-incompatible
changes. Patch releases within one minor line preserve the public source and
installed-library ABI. Consumers should review `MIGRATION.md` and rebuild when
moving between minors.

The installed `CrowdyCPPConfigVersion.cmake` uses CMake's
`SameMinorVersion` policy. A request for `0.16` may select a compatible newer
`0.16.x` package, but no `0.17.x` package is accepted as compatible merely
because both versions have major version `0`.
