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
Every test exits **77** (