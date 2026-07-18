# CrowdyCPP e2e coverage matrix

CrowdyCPP is the platform's canonical black-box end-to-end suite: it drives
the public APIs exactly as an external integrator would (no privileged
access) and, unlike the browser-first JS SDK, replicates over the **native
UDP** path directly to the replication servers. This document accounts for
every scenario in the platform's other e2e suites — each is `ported`,
`partial` (ported with a documented limitation), or `excluded` (with a
reason). Sources are named by their public product surface, not by any
internal repository.

Suites live in [`tests/e2e/`](../tests/e2e/); see that folder's README for
configuration and labels. `slow` and `optional` mark the non-default ctest
labels.

## CrowdyJS e2e suites

The TypeScript SDK's end-to-end suites are the primary parity target.

| CrowdyJS suite | CrowdyCPP suite | Status |
|---|---|---|
| `self-echo-actor` | `e2e_self_echo` | ported (native UDP) |
| `two-client-actor` | `e2e_two_client_actor` | ported (native UDP) |
| `two-client-voxel` | `e2e_two_client_actor` (voxel fan-out) + `e2e_voxels_permissions` | ported |
| `two-client-audio` | `e2e_two_client_audio` | ported |
| `two-client-messaging` | `e2e_two_client_messaging` | ported |
| `two-client-single-actor` | `e2e_two_client_messaging` (direct message) + `e2e_cross_server` | ported |
| `two-client-channel` | `e2e_two_client_messaging` (channel) | ported |
| `two-client-cross-app` | `e2e_cross_app` | ported (needs a 2nd app; skips otherwise) |
| `gamer-journey` | `e2e_gamer_journey` | ported |
| `new-app-default-access` | `e2e_studio_admin` (default-access scenario) | ported |
| `new-app-grid-creation` | `e2e_studio_admin` (grid-creation scenario) | ported |
| `studio-admin` | `e2e_studio_admin` | ported |
| `payments-economy` | `e2e_payments` | partial: create/list/idempotent-replay covered; capture excluded (no capture in a headless suite) |
| `operator-control-plane` | `e2e_operator` | partial: read-only control-plane queries; optional (needs an operator account) |
| `malicious-input` | `e2e_malicious_input` | ported |

## Game API SDK e2e (public `test/sdk` surface)

| Game API area | CrowdyCPP suite | Status |
|---|---|---|
| actors | `e2e_actors_crud` | ported |
| chunks | `e2e_chunks` | ported |
| voxels | `e2e_voxels_permissions` | ported (grant/deny on distinct chunks; history + rollback) |
| server-status | `e2e_gamer_journey` (`gameClientBootstrap`) + every UDP suite (`serverWithLeastClients`) | ported |
| state | `e2e_state_avatars` | ported (strict base64 round-trip) |
| teleport | `e2e_teleport` | ported |
| host discovery | `e2e_host_election` | ported |
| teams / channels | `e2e_teams_channels` | ported |
| game model + automations | `e2e_game_model`, plus every `e2e_kit_*` (model composition) | ported |
| udp-proxy (`connect`/`send*`/`udpNotifications`) | — | excluded: browser proxy path; CrowdyCPP replicates natively over UDP (see the parity matrix) |

## Management API e2e (public `test/auth` + `test/sdk`)

| Management area | CrowdyCPP suite | Status |
|---|---|---|
| auth / identities | `e2e_auth_identities` | ported (passwordless + password family + magic-link dev token) |
| users | `e2e_auth_identities` (`me`, profile) | ported |
| organizations | `e2e_studio_admin` | ported |
| apps | `e2e_studio_admin` | ported |
| app-access (tiers + grants) | `e2e_studio_admin` | ported |
| billing | `e2e_billing_quotas` | ported |
| quotas | `e2e_billing_quotas` | ported |
| usage | `e2e_billing_quotas` (summaries/projections) | ported |
| payments | `e2e_payments` | partial (no capture) |
| org-token-auth | `e2e_studio_admin` (create/update/revoke) | ported |
| cross-tenant | `e2e_cross_tenant` | ported |
| environments | `e2e_operator` (read-only) | partial: provisioning excluded (would allocate real infrastructure) |
| shared-environment | — | excluded: publishing mutates billing/runtime infra; not safe for a headless suite |

## Replication API behaviors (native UDP)

Reproduced black-box over the native transport, citing the public
[Replication API](https://docs.crowdedkingdoms.com/replication-api/intro)
docs (wire formats, HMAC, operations, troubleshooting).

| Behavior | CrowdyCPP suite | Status |
|---|---|---|
| client echo / self-echo | `e2e_self_echo` | ported |
| two-client spatial fan-out | `e2e_two_client_actor` | ported |
| spatial distance + decay | `e2e_spatial_distance` | ported |
| voxel update replication | `e2e_two_client_actor`, `e2e_chunk_store_live` | ported |
| single-actor (directed) message | `e2e_two_client_messaging`, `e2e_cross_server` | ported |
| channel messages | `e2e_two_client_messaging`, `e2e_durable_mirror` | ported |
| client-event replication | `e2e_stores_live` (EventRouter) | ported |
| negative auth (bad HMAC / wrong app / garbage) | `e2e_negative_auth` | ported |
| permission refresh (grant/revoke on a live session) | `e2e_permission_refresh` | ported (slow) |
| cross-server replication | `e2e_cross_server` | ported (optional; needs 2+ servers) |
| soak / sustained two-client | `e2e_soak_two_clients` | ported (slow) |
| `COMMAND_RECONNECT` load-shed | — | excluded: server-side trigger; client handling is covered by the offline `replication_test` fake-server unit test |
| server-notification inject; runtime-permission schema | — | excluded: internal (server-to-server / operator) surfaces, not part of the public client contract |

## SDK-managed data structures (no JS parity source; new coverage)

CrowdyCPP's WorldSession stores are exercised live because they are the layer
a shipped game renders from.

| Structure | CrowdyCPP suite |
|---|---|
| local actor loop + acks, remote-actor lanes, EventRouter, AndWait | `e2e_stores_live` |
| chunk cache: hydrate, optimistic edit + write-back, seed, prune, flush | `e2e_chunk_store_live` |
| SaveState / AvatarState stores, ContainerMirror (notify-to-pull), UUID persistence | `e2e_durable_mirror` |

## Game Kit (no JS e2e source; mirrors the `smoke-mmo` / `smoke-plots` samples)

One suite per genre layer: deploy the blueprint as admin, play as a
player, and assert the policy denials resolve as results (`success:false`),
never exceptions.

| Layer | CrowdyCPP suite |
|---|---|
| inventory | `e2e_kit_inventory` |
| lockable objects | `e2e_kit_objects` |
| plots (land + enforced grid grants) | `e2e_kit_plots` |
| NPCs / automations | `e2e_kit_npcs` |
| economy | `e2e_kit_economy` |
| progression | `e2e_kit_progression` |
| loot | `e2e_kit_loot` |
| quests | `e2e_kit_quests` |
| combat | `e2e_kit_combat` |
| matches (+ notify-to-pull) | `e2e_kit_matches` |
| decks (hidden information) | `e2e_kit_decks` |
| world sim | `e2e_kit_worldsim` |
| social (parties/guilds) | `e2e_kit_social` |
| leaderboards | `e2e_kit_leaderboards` |
| feature gates | `e2e_kit_features` |
| composite journey | `e2e_kit_journey` |

## Findings surfaced by writing these suites

Building the suites against a live deployment exposed a few contract details
worth recording (tracked for follow-up; the suites adapt to the observed
public behavior):

- **Game Kit blueprint selectors** — the combat status-effect tick and the
  worldsim/economy regen automations ship selector shapes the current
  automation resolver does not bind (`bindAs` ref without `pick`; `selfWhere`
  referencing `self.*`). These originate in the shared blueprint definitions
  (the C++ builders reproduce the JS builders exactly, so both SDKs carry
  them); the suites re-upsert corrected automations and the defect is flagged
  for a coordinated blueprint fix in both SDKs.
- **Admin/server-scope callers bypass invoke-policy conditions** — a
  server-scope owner call is not bound by a function's `condition` guard
  (e.g. the loot "no re-roll" guard). This is the documented authority model;
  the loot runtime doc comment is being updated to say so.
- **`gameApps().userPermissions` is admin-only** — players cannot read their
  own grid grants; post-grant verification in the plot suite goes through the
  owner client.
- **`createGrid` reports success via an `error: "NO_ERROR"` sentinel** rather
  than a null error field; the world-grid assignment lands lazily on first
  UDP touch.

These are captured here so the matrix stays honest about what the live
platform does, not only what the docs describe.
