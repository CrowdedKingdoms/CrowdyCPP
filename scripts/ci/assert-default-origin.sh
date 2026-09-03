#!/usr/bin/env bash
# Refuse a generated default origin that names a tier other than the one being released.
#
# resolve-release-tier.sh answers "does this tag's COMMIT belong to the branch the tag names".
# assert-tag-version.sh answers "is the tag's NUMBER the number that commit builds". Neither
# reads what the commit CONTAINS, so a prod tag over a tree whose default_origin.hpp still
# says `test` passes both and ships a release that dials the test tier.
#
# THE RECEIPTS ARE FROM 2026-09-02, AND BOTH SDKS HAVE THEM. Promoting test onto prod left
# include/crowdy/default_origin.hpp declaring `test` on prod, with no conflict reported, in
# this repo and in CrowdyJS on the same day. CrowdyJS's half also reached the registry: 15.4.0
# published `15.4.0-dev.1` and `15.4.0-test.1` whose bundled default declared prod. Here it
# would be worse, because there is no registry and no dist-tag to correct afterwards -- a
# consumer clones a ref, so THE TAG IS THE ARTIFACT and a bad one stands until a new tag exists.
#
# WHY A CHECK AND NOT A MERGE STRATEGY. Measured in a scratch repository, not reasoned about:
# the carry is silent whenever the merge base already holds the DESTINATION's value and the
# destination has not re-committed the file while the source has. Only one side changed, so git
# resolves it trivially and reports success. A .gitattributes merge driver cannot help, twice
# over -- git never consults a driver for a one-sided change, which is exactly the dangerous
# case (zero invocations were logged while a carry happened), and .gitattributes can NAME a
# driver but not ship it, so merge.<name>.driver reads as unset in a fresh clone and git falls
# back to its default silently. A CI runner has no driver at all.
#
# The HOST half -- whether `dev` should be one host or another -- needs the operator's tier
# table and stays in check-sdk-default-origin.mjs. What is here is hermetic: which tier the
# file names versus which tier is being released, plus the file's agreement with itself.
#
# WHEN THE INPUT IS MISSING THIS FAILS, like its siblings. Not being able to read the tier is a
# third outcome and the tempting one to call a pass is the one that lets a release out
# unchecked.
#
# Usage: scripts/ci/assert-default-origin.sh <dev|test|prod> [path/to/default_origin.hpp]
#        scripts/ci/assert-default-origin.sh --from-ref [path]   # tier from the ref, for CI
#
# --from-ref prefers GITHUB_BASE_REF, so on a promotion pull request this is judged by the
# DESTINATION's rule before the merge rather than after it. A ref carrying no tier -- a feature
# branch -- is SKIPPED and said to be skipped; only dev/test/prod carry a tier rule.
#
# Run scripts/ci/assert-default-origin.test.sh to see it refuse.
set -euo pipefail

WANT=${1-}
HEADER=${2-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/include/crowdy/default_origin.hpp}

die() {
  echo "::error::$*" >&2
  exit 1
}

is_tier() {
  case "$1" in dev | test | prod) return 0 ;; *) return 1 ;; esac
}

# The tier a ref publishes for, or empty when it carries none. A tag is `dev/v0.29.1`, so the
# tier is the segment before the slash; a branch is the whole name.
tier_of_ref() {
  local bare=${1#refs/heads/}
  bare=${bare#refs/tags/}
  local head=${bare%%/*}
  if is_tier "$head"; then printf '%s' "$head"; fi
}

[ -n "$WANT" ] || die "no tier given (argument 1: dev, test, prod, or --from-ref)"

SOURCE="argument"
if [ "$WANT" = "--from-ref" ]; then
  ref=${GITHUB_BASE_REF:-}
  SOURCE="GITHUB_BASE_REF (the pull request base)"
  if [ -z "$ref" ]; then
    ref=${GITHUB_REF_NAME:-${GITHUB_REF:-}}
    SOURCE="GITHUB_REF_NAME"
  fi
  if [ -z "$ref" ]; then
    ref=$(git -C "$(dirname "$HEADER")" rev-parse --abbrev-ref HEAD 2>/dev/null || true)
    [ "$ref" = "HEAD" ] && ref=""
    SOURCE="the checked-out branch"
  fi
  if [ -z "$ref" ]; then
    # In CI, resolving no ref at all means the workflow did not give this gate its input, and a
    # gate that cannot see its input must not report success.
    [ -n "${GITHUB_ACTIONS:-}" ] &&
      die "no ref to judge against (GITHUB_BASE_REF, GITHUB_REF_NAME and GITHUB_REF are all unset, and there is no checked-out branch). In CI that is a misconfiguration, not a branch without a tier."
    die "no ref and no branch to derive a tier from; pass the tier explicitly"
  fi
  WANT=$(tier_of_ref "$ref")
  if [ -z "$WANT" ]; then
    # Still verify the file agrees with itself, then report the skip in those words.
    [ -r "$HEADER" ] || die "cannot read $HEADER, which is generated and load-bearing; a missing file is not an empty one"
    declared=$(sed -n 's/.*kDefaultTier *= *"\([a-z]*\)".*/\1/p' "$HEADER" | head -n1)
    echo "tier=skipped ref='$ref' declares='${declared:-unparseable}'"
    echo "ref '$ref' ($SOURCE) carries no tier, so the comparison was SKIPPED. A pull request into dev, test or prod is judged by that base."
    exit 0
  fi
fi

is_tier "$WANT" || die "'$WANT' is not one of dev, test, prod"
[ -r "$HEADER" ] || die "cannot read $HEADER, so the declared tier is unknown; refusing rather than assuming the release is right"

declared=$(sed -n 's/.*kDefaultTier *= *"\([a-z]*\)".*/\1/p' "$HEADER" | head -n1)
host=$(sed -n 's/.*kDefaultHost *= *"\([^"]*\)".*/\1/p' "$HEADER" | head -n1)
http=$(sed -n 's/.*kDefaultHttpOrigin *= *"\([^"]*\)".*/\1/p' "$HEADER" | head -n1)
ws=$(sed -n 's/.*kDefaultWsOrigin *= *"\([^"]*\)".*/\1/p' "$HEADER" | head -n1)

[ -n "$declared" ] || die "no kDefaultTier line in $HEADER; refusing rather than passing on an unreadable tier"
[ -n "$host" ] || die "no kDefaultHost line in $HEADER; refusing rather than passing on an unreadable host"

# Internal agreement. Needs no tier table, and catches the half-edit a generated file invites:
# moving the tier and leaving the host behind, or composing a scheme by hand.
[ "$http" = "https://$host" ] ||
  die "kDefaultHttpOrigin is '$http' but the host is '$host': a scheme is composed, so it must be https://<host>"
[ "$ws" = "wss://$host" ] ||
  die "kDefaultWsOrigin is '$ws' but the host is '$host': it must be wss://<host>"

# The tier must appear as a label in the host, asserted structurally so this script carries no
# hostname literal of its own.
case ".$host." in
  *".$declared."*) ;;
  *) die "$HEADER declares tier '$declared' and host '$host', and that host carries no '$declared' label. One of the two was edited without the other; regenerate the file." ;;
esac

if [ "$declared" != "$WANT" ]; then
  die "$HEADER declares tier '$declared' and this release is '$WANT' ($SOURCE). A branch ships one tier's artifact, and this SDK has no registry -- consumers clone the ref, so the tag IS the artifact. Regenerate for the destination tier (sync-client-origins.mjs --write --tier $WANT in the operator wrapper) rather than hand-editing. If a promotion produced this, note that it can rewrite the file with NO conflict, so a clean merge here deserves more suspicion than a conflicting one."
fi

echo "tier=$declared"
