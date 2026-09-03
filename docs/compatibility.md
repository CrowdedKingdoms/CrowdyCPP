# SDK and Game API compatibility

CrowdyCPP 0.29.1 passes the strict portable-parity gate against CrowdyJS
**15.4.0**. The gate pins CrowdyJS commit
`fb8acc6567e7425a94ec0f379220a7baba9032f6` (`crowdyjsParityTarget` in
`package.json`); see [`parity-matrix.md`](parity-matrix.md) for the generated
method-by-method evidence. Native equivalents and browser exclusions remain
intentional, so this does not claim identical transports or browser behavior.

**The pinned commit must be one this tier has actually been promoted.** Before
2026-09-02 all three branches pinned `8937075…`, a merge commit created on
CrowdyJS `prod` and therefore reachable from `prod` and from neither `dev` nor
`test` — so the `dev` and `test` lines were claiming parity against another
tier's commit. `check-parity-target-tier.mjs` refuses that now.

The rule is **reachability from this tier's branch**, which is the same thing as
"this commit has been promoted to me". Because promotions merge forward, one
commit satisfies every tier once it has travelled the ladder: `fb8acc6…` is
`dev/v15.4.0` and is an ancestor of `dev`, `test` and `prod` alike. So this line
does not need to differ per tier and the value promotes forward like any other.

The asymmetry is deliberate and is not a gap. `prod` may pin a commit that also
lives on `dev`, because reaching `prod` means it was promoted there; `dev` may
not pin a `prod`-only merge commit, because that commit has never been promoted
back. Pin a **commit**, never a branch head — an ancestor keeps the generated
fixtures reproducible, whereas a moving head is the "same-version moving branch"
the pin exists to prevent. Moving the version is a separate, deliberate act; see
[`release-checklist.md`](release-checklist.md).

| Surface | CrowdyCPP 0.29.1 | CrowdyJS 15.4.0 | Required public API generation |
|---|---|---|---|
| Core Management and Game GraphQL | Supported | Supported | Current published Management + Game SDL |
| Native UDP replication | Direct native transport | Browser GraphQL UDP proxy | Current Replication API |
| Generic GraphQL WebSocket | `GraphQLSubscriptionClient` | `graphql-ws` | `graphql-transport-ws` endpoint |
| Game-model container feed | Typed `gameModel().containerChanged` | Typed `containerChanged` | Game API 2026-07+ |
| App-scoped player counts | Typed snapshot + change stream | Typed snapshot + change stream | Game API 2026-07-24+ |
| Keyed container ensure/filter | `ensureContainer`, `bindingKey` filter | `ensureContainer`, `bindingKey` filter | Game API 2026-07-24+ |
| App listing-version administration | `marketplace().appListingVersions` | Typed listing-version methods | Management API 2026-07-24+ |
| Crowdy Studio projects/runtime | Headless native controller, typed diagnostics/wallet observation | Browser/headless controller | Game API project/runtime roots; durable checkpoint mutations require an injected bridge |
| Crowdy Studio pane layout | Headless controller with injected storage | Headless controller with browser-local default storage | None |
| Native Studio integration | Owned editor/layout/runtime/host/Agent/control assembly with explicit maintenance scheduling | Browser Studio composition | Project/runtime roots plus `crowdy.studio-agent/1` when Agent support is enabled |
| Agentic Studio HTTP + event stream | Typed controller, GraphQL-WS replay/gap-fill | Typed controller and transport | `crowdy.studio-agent/1` |
| Local Play/Studio tools | Native player-host + closed typed dispatcher | Browser dispatcher + player host | Agent descriptor contract v1 |

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
splitters, embed panel/dock/HUD/styles/focus handling, and worker-entry
packaging. CLIENT
artifact-byte decoding is portable and is available through
`playerCompute().artifactBytes(...)` and
`marketplace().clientArtifactBytes(...)`.
Portable gaps, unclassified differences, and stale classifications are zero.

Known coordinated limitation: the pinned CrowdyJS/CrowdyCPP Game Kit
blueprints retain combat status-tick and worldsim node/crop selector forms that
the deployed Game API does not currently execute. Runtime helpers remain
available, but those automatic schedules are not claimed as supported until
the shared blueprint/server contract changes in both SDKs.

## CrowdyCPP 0.x source and ABI policy

Until 1.0, each minor release may contain source-incompatible or ABI-incompatible
changes. Patch releases within one minor line preserve the public source and
installed-library ABI. Consumers should review `MIGRATION.md` and rebuild when
moving between minors.

The installed `CrowdyCPPConfigVersion.cmake` uses CMake's
`SameMinorVersion` policy. A request for `0.16` may select a compatible newer
`0.16.x` package, but no `0.17.x` package is accepted as compatible merely
because both versions have major version `0`.
