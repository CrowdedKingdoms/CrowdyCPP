// The input log (inputLogSessions / inputLogMessages), read by the app's owner.
//
// Read-only: it lists whatever the app has recorded, which may be nothing (turning replay logging
// on needs a funded wallet, and this suite leaves the app's settings alone). What it proves on a
// live Game API: the sessions document's `missingRecords` selection is accepted (an API before
// the release after v2.40.2 refuses it), one session's inputs page when there is one, and a
// cancelled token sends nothing and is reported cancelled.
// Reference: https://docs.crowdedkingdoms.com/replication-api/input-logging
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& game = e2e::ownerGame(cfg);

  E2E_SUBTEST("sessions selects missingRecords, and the Game API accepts it");
  const graphql::Json page = game.inputLog().sessions(cfg.appId, 5);
  E2E_CHECK(page["edges"].isArray());
  E2E_CHECK(page["pageInfo"]["hasNextPage"].isBool());
  std::string gameTokenId;
  page["edges"].forEach([&](graphql::Json edge) {
    // Present on every node: a count, or null for a session recorded before the count existed.
    E2E_CHECK(edge["node"]["missingRecords"].ok());
    if (gameTokenId.empty()) gameTokenId = edge["node"]["gameTokenId"].asString();
  });
  std::printf("   %zu session(s) on the first page\n", page["edges"].size());

  if (!gameTokenId.empty()) {
    E2E_SUBTEST("messages pages one session");
    const graphql::Json messages = game.inputLog().messages(cfg.appId, gameTokenId, 20);
    E2E_CHECK(messages["edges"].isArray());
    E2E_CHECK(messages["pageInfo"]["hasNextPage"].isBool());
  }

  E2E_SUBTEST("a token cancelled before the call sends nothing and throws CrowdyCancelledError");
  graphql::CancelToken cancelled;
  cancelled.cancel();
  bool threw = false;
  try {
    (void)game.inputLog().sessions(cfg.appId, 5, {}, {}, cancelled);
  } catch (const graphql::CrowdyCancelledError& e) {
    threw = true;
    E2E_CHECK(e.code() == "CANCELLED");
  }
  E2E_CHECK(threw);

  E2E_SUBTEST("a token nobody cancels reads normally");
  graphql::CancelToken idle;
  const graphql::Json again = game.inputLog().sessions(cfg.appId, 1, {}, {}, idle);
  E2E_CHECK(again["edges"].isArray());

  std::puts("e2e_input_log OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
