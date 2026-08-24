// The generated default origin, and the ALL-OR-NOTHING rule around it.
//
// A default is load-bearing during a rollout: while the branches are
// mid-migration this is what an unconfigured consumer gets. So it is asserted in
// BOTH directions. Asserting only that the default applies would go on passing
// after the guard that stops it applying to a PARTIALLY configured client had
// been deleted — and that guard is the whole reason a default is safe here. A
// client whose HTTP is on the caller's host and whose websocket is on the tier
// default is one session across two origins, looking connected throughout.
//
// NO HOSTNAME IS WRITTEN HERE. The expectation is the generated constant, so
// this file cannot go stale when the tier table moves; the wrapper's
// check-sdk-default-origin.mjs is what compares that constant to the declared
// origin for this branch's tier.
#include <cstdio>
#include <string>

#include "crowdy/client.hpp"
#include "crowdy/default_origin.hpp"
#include "test_util.hpp"

namespace {

void testTheGeneratedConstantsAreConsistent() {
  CHECK(std::string(crowdy::kDefaultTier).size() > 0);
  CHECK(std::string(crowdy::kDefaultHost).size() > 0);
  CHECK(std::string(crowdy::kDefaultHttpOrigin) ==
        "https://" + std::string(crowdy::kDefaultHost));
  CHECK(std::string(crowdy::kDefaultWsOrigin) ==
        "wss://" + std::string(crowdy::kDefaultHost));
}

void testAnUnconfiguredClientTakesTheDefault() {
  crowdy::ClientConfig config;
  crowdy::CrowdyClient client(std::move(config));
  CHECK(client.config().httpUrl == std::string(crowdy::kDefaultHttpOrigin));
  CHECK(client.config().wsUrl == std::string(crowdy::kDefaultWsOrigin));
  // The default IS the shared multivalue name, which is exactly what
  // discoveryUrl wants — leaving it empty gives an unconfigured client no way
  // back from an instance that stops answering.
  CHECK(client.config().discoveryUrl == std::string(crowdy::kDefaultHttpOrigin));
}

void testAConfiguredClientGetsNothingInvented() {
  crowdy::ClientConfig config;
  config.httpUrl = "https://game.invalid";
  crowdy::CrowdyClient client(std::move(config));
  CHECK(client.config().httpUrl == "https://game.invalid");
  // The websocket must stay EMPTY rather than inherit the tier default.
  CHECK(client.config().wsUrl.empty());
  CHECK(client.config().discoveryUrl.empty());
}

void testASocketOnlyClientIsAlsoLeftAlone() {
  crowdy::ClientConfig config;
  config.wsUrl = "wss://game.invalid";
  crowdy::CrowdyClient client(std::move(config));
  CHECK(client.config().wsUrl == "wss://game.invalid");
  CHECK(client.config().httpUrl.empty());
}

}  // namespace

int main() {
  testTheGeneratedConstantsAreConsistent();
  testAnUnconfiguredClientTakesTheDefault();
  testAConfiguredClientGetsNothingInvented();
  testASocketOnlyClientIsAlsoLeftAlone();
  std::printf("default_origin_test: 4 case(s) ok — tier=%s host=%s\n",
              crowdy::kDefaultTier, crowdy::kDefaultHost);
  return 0;
}
