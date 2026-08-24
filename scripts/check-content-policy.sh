#!/usr/bin/env bash
# Public content policy check: CrowdyCPP is a public repository. Nothing in it
# may reference private repositories, internal infrastructure, or internal
# server-to-server protocol details. CI fails if any denylisted term appears
# in tracked files.
set -euo pipefail

cd "$(dirname "$0")/.."

DENYLIST=(
  'cks-udp-api'
  'cks-michael-root'
  'cks-project-root'
  'MessageType\.hpp'
  'wire-protocol-reference'
  'P2P_SECRET'
  'P2P_TOKEN'
  'CHANNEL_MUTATION'
  'peer port'
  'port 9081'
  ':9081'
  'buddydev'
  'BUDDY_BUILDER'
  'dev-run-buddy'
)

# THE SCHEMA EXEMPTION IS GONE, and the reason it existed is worth keeping.
# `schema.gql` was excluded on the grounds that it is a verbatim copy of the
# PUBLISHED SDL and that "description fixes belong server-side". Both halves were
# true, and together they made the exemption permanent: it was the correct
# diagnosis with nobody assigned to act on it, so for as long as it stood this
# gate could not object to the two private-repo names actually in the file, and
# nothing else was counting them either. The names were removed at the source --
# in the API's own GraphQL decorators, so every downstream copy inherits the fix
# rather than carrying its own exclusion -- on 2026-08-23. The snapshot is clean,
# so the exemption is no longer load-bearing and the file is now in the corpus.
#
# An exemption for a file this gate cannot fix is really a note that somebody
# should fix it elsewhere. Prefer fixing it elsewhere.
fail=0
for term in "${DENYLIST[@]}"; do
  # Search tracked files only. That is the right corpus HERE, unlike in the
  # TypeScript SDK: this package is consumed as source, so what a user receives
  # is the tracked tree. CrowdyJS publishes a built `dist/` that git ignores, so
  # its equivalent gate has to walk the filesystem instead.
  if hits=$(git grep -nIE --untracked "$term" -- \
      ':!scripts/check-content-policy.sh' \
      2>/dev/null); then
    echo "DENYLISTED TERM '$term' found:" >&2
    echo "$hits" >&2
    fail=1
  fi
done

if [[ $fail -ne 0 ]]; then
  echo "Content policy check FAILED — remove internal references before publishing." >&2
  exit 1
fi
echo "Content policy check passed."
