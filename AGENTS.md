# CrowdyCPP agent guidance

CrowdyCPP is a standalone public C++20 SDK. A normal configure, build, install,
or unit-test run must not require network access, Node, CrowdyJS, or private
platform repositories. Schema and generated artifacts are committed.

## Public-surface maintenance

When a Management/Game GraphQL surface changes:

1. Run `npm ci` for maintainer-only schema tooling.
2. Refresh `schema.gql` with `scripts/schema-sync.mjs` from the coordinated
   Management and Game SDLs.
3. Run `node scripts/codegen.mjs`; commit `schema.gql`,
   `include/crowdy/generated/enums.hpp`, and
   `include/crowdy/generated/operations.hpp` together.
4. Run `node scripts/codegen.mjs --check`.
5. Compare the reviewed CrowdyJS checkout with
   `node tools/parity/parity.mjs --crowdyjs <path> --write
   docs/parity-matrix.md`, then run the same command with `--check`.
6. If agent contracts changed, build CrowdyJS and run
   `node tools/parity/agent-fixtures.mjs --crowdyjs <path>`. Use `--write`
   only for an intentional coordinated fixture update.
7. Run the blueprint structural gate documented in `README.md`.

The CI CrowdyJS commit is deliberately pinned. Update that SHA, the generated
matrix, and any descriptor/preemption fixtures in one reviewed change.

Parity classifications are strict:

- `portable gap` means missing implementation that remains visible and must be
  removed as work lands;
- `native equivalent` is allowed only when CrowdyCPP provides the same contract
  through its native architecture;
- `browser exclusion` is allowed only for browser/UI/worker-specific behavior.

Do not label planned native WebSockets, Crowdy Studio, agent control, leases, or
player-host work as browser-only. New differences and stale classifications
must fail the baseline gate. `parity.mjs --strict` must pass before declaring
strict parity complete.
