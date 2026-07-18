# CrowdyCPP end-to-end suites

Black-box e2e tests against a live Crowded Kingdoms deployment. Everything is
unprivileged: provisioning happens through the public Management API the way a
real integrator does it (owner sign-in -> access tier -> `grantAppAccess`) —
no database access anywhere. Replication traffic runs over **native UDP**
directly to the replication servers, so these suites exercise the real
shipping client path.

Coverage accounting lives in [docs/e2e-coverage.md](../../docs/e2e-coverage.md):
every scenario from the platform's other e2e suites is mapped to a CrowdyCPP
suite or carries an explicit exclusion reason.

## Configuration

| Variable | Required | Purpose |
|---|---|---|
| `CROWDY_E2E_MANAGEMENT_URL` | yes | Management API base URL |
| `CROWDY_E2E_HTTP_URL` | no | Game API base URL (falls back to the minted `gameApiUrl`) |
| `CROWDY_E2E_EMAIL` | yes | base email; suites derive fresh accounts by plus-addressing |
| `CROWDY_E2E_APP_ID` | yes | the app under test |
| `CROWDY_E2E_OWNER_EMAIL` | yes* | account with `manage_apps` + `manage_access_tiers` on the app (entitles players, deploys kit blueprints) |
| `CROWDY_E2E_APP_ID_2` | no | second app on the same deployment (cross-app isolation) |
| `CROWDY_E2E_OPERATOR_EMAIL` | no | `is_operator` account (operator read-only suite) |
| `CROWDY_E2E_MULTI_SERVER=1` | no | deployment runs 2+ replication servers (cross-server suite) |
| `CROWDY_E2E_SLOW=1` | no | enable long-running suites (soak, cache-TTL waits) |

\* a few legacy suites can run with pre-entitled fixed accounts
(`CROWDY_E2E_EMAIL`/`_EMAIL_2`) instead, but the owner unlocks everything.

The management server must run with `DEV_AUTH_BYPASS` (the suites sign in with
`devLogin`; the auth suite also exercises the magic-link dev-token flow).
Every test exits **77** (ctest `SKIP_RETURN_CODE`) when its required
variables are unset, so an unconfigured checkout reports skips, not failures.

## Running

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# default suites (fast; skip when unconfigured)
ctest --test-dir build -L e2e --output-on-failure

# long-running suites (soak, permission-cache TTL) — also needs CROWDY_E2E_SLOW=1
ctest --test-dir build -L e2e_slow --output-on-failure

# optional suites needing extra deployment features (operator account,
# multiple replication servers) — gated on their own env vars
ctest --test-dir build -L e2e_optional --output-on-failure
```

Run a single suite directly for its per-subtest output:

```bash
./build/tests/e2e/e2e_two_client_actor
```

## Labels

| Label | Suites | Gate |
|---|---|---|
| `e2e` | everything not below | env config |
| `e2e_slow` | `e2e_permission_refresh`, `e2e_soak_two_clients` | `CROWDY_E2E_SLOW=1` |
| `e2e_optional` | `e2e_cross_server`, `e2e_operator` | `CROWDY_E2E_MULTI_SERVER=1` / `CROWDY_E2E_OPERATOR_EMAIL` |

## Notes for reruns

- Suites derive fresh accounts (plus-addressed with a per-run suffix) and
  use unique kit blueprint prefixes, so back-to-back runs never collide on a
  shared app.
- Each suite owns a disjoint chunk-coordinate band (base
  `{100000..500000 + suite*100, 0, ...}`) so parallel suites don't cross
  spatial fan-out.
- `assignServer failed: No available servers found` during connect is
  transient on small deployments (server-status heartbeats briefly lapse) and
  is absorbed by the harness's assignment retry — not a failure.
- On a busy shared deployment, `Connection::stats().hmacFailures` may be
  nonzero: a concurrent player's foreign-signed frame fanning into a shared
  chunk is correctly dropped by verification. Suites treat this as a
  diagnostic, asserting on their own traffic's integrity instead.