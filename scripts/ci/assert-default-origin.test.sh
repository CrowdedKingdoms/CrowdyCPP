#!/usr/bin/env bash
# Proves assert-default-origin.sh refuses, and — just as important — that it accepts.
#
# A check that always fails passes every refusal case, so both directions or neither is
# evidence. Every tier is accepted in its correct form here for exactly that reason.
#
# The missing-input cases are here because "could not read the tier" is a third outcome that
# gates in this org have quietly resolved to "pass"; these pin it to "fail". The skip case is
# separated from them on purpose: a feature branch legitimately has no tier rule, and the
# difference between "skipped and said so" and "passed" is the whole point.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$HERE/assert-default-origin.sh"
[ -x "$SCRIPT" ] || { echo "not executable: $SCRIPT" >&2; exit 1; }

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
no() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A generated header for a tier, using an example domain so this fixture carries no
# first-party hostname.
plant() {
  local tier=$1 host=$2 out=$3
  {
    printf 'namespace crowdy {\n'
    printf 'inline constexpr const char* kDefaultTier = "%s";\n' "$tier"
    printf 'inline constexpr const char* kDefaultHttpOrigin = "https://%s";\n' "$host"
    printf 'inline constexpr const char* kDefaultWsOrigin = "wss://%s";\n' "$host"
    printf 'inline constexpr const char* kDefaultHost = "%s";\n' "$host"
    printf '}  // namespace crowdy\n'
  } >"$out"
}

for t in dev test prod; do plant "$t" "api.$t.example.com" "$WORK/$t.hpp"; done
plant prod "api.test.example.com" "$WORK/tier-host-disagree.hpp"
printf 'inline constexpr const char* kNothingUseful = "x";\n' >"$WORK/no-tier.hpp"
{
  printf 'inline constexpr const char* kDefaultTier = "dev";\n'
  printf 'inline constexpr const char* kDefaultHttpOrigin = "https://api.prod.example.com";\n'
  printf 'inline constexpr const char* kDefaultWsOrigin = "wss://api.dev.example.com";\n'
  printf 'inline constexpr const char* kDefaultHost = "api.dev.example.com";\n'
} >"$WORK/scheme-mismatch.hpp"

# expect_refusal <name> <substring the message must contain> [args...]
expect_refusal() {
  local name=$1 want=$2 out rc
  shift 2
  out=$(env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_REF_NAME -u GITHUB_BASE_REF "$SCRIPT" "$@" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then
    no "$name: expected refusal, got exit 0 and: $out"
  elif ! grep -qF -- "$want" <<<"$out"; then
    no "$name: refused (good) but message lacks '$want': $out"
  else
    ok "$name"
  fi
}

# expect_accept <name> <substring stdout must contain> [args...]
expect_accept() {
  local name=$1 want=$2 out rc
  shift 2
  out=$(env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_REF_NAME -u GITHUB_BASE_REF "$SCRIPT" "$@" 2>&1)
  rc=$?
  if [ $rc -ne 0 ]; then
    no "$name: expected acceptance, got exit $rc and: $out"
  elif ! grep -qF -- "$want" <<<"$out"; then
    ok "$name (accepted, but stdout lacked '$want': $out)"
    FAIL=$((FAIL + 1))
    PASS=$((PASS - 1))
    echo "  ^ counted as a failure"
  else
    ok "$name"
  fi
}

echo "assert-default-origin.sh"
echo
echo "refusals:"

# The promotion carry, in both the directions it has actually happened.
expect_refusal "a test default released as prod -> refused" \
  "declares tier 'test' and this release is 'prod'" prod "$WORK/test.hpp"

expect_refusal "a prod default released as dev -> refused (this is 15.4.0's shape)" \
  "declares tier 'prod' and this release is 'dev'" dev "$WORK/prod.hpp"

expect_refusal "a dev default released as test -> refused" \
  "declares tier 'dev' and this release is 'test'" test "$WORK/dev.hpp"

# Half-edits.
expect_refusal "tier and host that disagree -> refused" \
  "carries no 'prod' label" prod "$WORK/tier-host-disagree.hpp"

expect_refusal "an http origin that is not the host -> refused" \
  "a scheme is composed" dev "$WORK/scheme-mismatch.hpp"

# Missing and malformed input: each one a plausible "pass quietly" bug.
expect_refusal "no tier argument -> refused" \
  "no tier given" "" "$WORK/dev.hpp"

expect_refusal "a tier that is not a tier -> refused" \
  "is not one of dev, test, prod" staging "$WORK/dev.hpp"

expect_refusal "unreadable header -> refused, not passed" \
  "cannot read" dev "$WORK/does-not-exist.hpp"

expect_refusal "header with no kDefaultTier -> refused, not passed" \
  "no kDefaultTier line" dev "$WORK/no-tier.hpp"

echo
echo "acceptances:"

# Every tier in its correct form, so refusal above is not just "always fails".
for t in dev test prod; do
  expect_accept "a $t default released as $t -> accepted" "tier=$t" "$t" "$WORK/$t.hpp"
done

echo
echo "--from-ref:"

# A tier branch, a tier tag, and a promotion pull request judged by its BASE.
out=$(env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_BASE_REF GITHUB_REF_NAME=test "$SCRIPT" --from-ref "$WORK/test.hpp" 2>&1)
[ $? -eq 0 ] && grep -qF 'tier=test' <<<"$out" && ok "a test branch with a test default -> accepted" || no "a test branch with a test default: $out"

out=$(env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_BASE_REF GITHUB_REF_NAME=prod/v0.29.1 "$SCRIPT" --from-ref "$WORK/prod.hpp" 2>&1)
[ $? -eq 0 ] && grep -qF 'tier=prod' <<<"$out" && ok "a prod/vX.Y.Z tag with a prod default -> accepted" || no "a prod tag with a prod default: $out"

out=$(env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_BASE_REF GITHUB_REF_NAME=prod/v0.29.1 "$SCRIPT" --from-ref "$WORK/test.hpp" 2>&1)
if [ $? -eq 0 ]; then no "a prod tag with a test default: expected refusal, got: $out"; else ok "a prod/vX.Y.Z tag with a test default -> refused"; fi

# The destination decides, not the source. This is the case that makes a promotion PR fail
# BEFORE the merge instead of after it.
out=$(env -u GITHUB_ACTIONS -u GITHUB_REF GITHUB_BASE_REF=test GITHUB_REF_NAME=dev "$SCRIPT" --from-ref "$WORK/dev.hpp" 2>&1)
if [ $? -eq 0 ]; then no "a PR from dev into test carrying dev's default: expected refusal, got: $out"; else ok "a PR based on test judges the BASE, so dev's default is refused"; fi

out=$(env -u GITHUB_ACTIONS -u GITHUB_REF GITHUB_BASE_REF=test GITHUB_REF_NAME=dev "$SCRIPT" --from-ref "$WORK/test.hpp" 2>&1)
[ $? -eq 0 ] && ok "a PR based on test with test's default -> accepted" || no "a PR based on test with test's default: $out"

# A feature branch is skipped and SAYS it was skipped.
out=$(env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_BASE_REF GITHUB_REF_NAME=michael/some-work "$SCRIPT" --from-ref "$WORK/dev.hpp" 2>&1)
if [ $? -ne 0 ]; then
  no "a feature branch: expected a skip, got: $out"
elif ! grep -qF 'SKIPPED' <<<"$out"; then
  no "a feature branch was passed rather than reported as skipped: $out"
else
  ok "a feature branch is skipped, and reported as skipped"
fi

# In CI, no resolvable ref is a misconfiguration rather than a pass.
out=$(env -u GITHUB_REF -u GITHUB_REF_NAME -u GITHUB_BASE_REF GITHUB_ACTIONS=true "$SCRIPT" --from-ref /nonexistent/dir/default_origin.hpp 2>&1)
if [ $? -eq 0 ]; then no "CI with no ref: expected refusal, got: $out"; else ok "CI with no resolvable ref -> refused"; fi

echo
echo "the real repository:"

# The default path is the code path CI uses. A fix to it would otherwise break the only caller
# with every test still green.
REPO=$(cd "$HERE/../.." && pwd)
real_tier=$(sed -n 's/.*kDefaultTier *= *"\([a-z]*\)".*/\1/p' "$REPO/include/crowdy/default_origin.hpp" | head -n1)
out=$(cd "$REPO" && env -u GITHUB_ACTIONS -u GITHUB_REF -u GITHUB_REF_NAME -u GITHUB_BASE_REF "$SCRIPT" "$real_tier" 2>&1)
if [ $? -ne 0 ]; then
  no "this repository's own header, default path -> accepted: got: $out"
else
  ok "this repository's own header ('$real_tier'), default path -> accepted"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
