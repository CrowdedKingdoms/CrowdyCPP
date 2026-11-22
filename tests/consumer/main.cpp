#include <string>
#include <vector>

#include <crowdy/crowdy.hpp>

// Linked against the INSTALLED package, so this is the only place that can catch
// a public header which is installed but not reachable from the umbrella.
// estate.hpp was exactly that: it shipped, it compiled standalone under CI's
// per-header sweep, and a consumer doing `#include <crowdy/crowdy.hpp>` still
// could not see isSameEstate.
int main() {
  const auto endpoint =
      crowdy::graphql::normalizeGraphQLWebSocketUrl(
          "https://api.example.test");
  if (!endpoint.ok() ||
      endpoint.value() != "wss://api.example.test/graphql") {
    return 1;
  }

  // The endpoint-mobility surface, reached through the umbrella only.
  if (!crowdy::graphql::isSameEstate("https://ck.prod.example.test",
                                     "https://ck-or.prod.example.test")) {
    return 1;
  }
  if (crowdy::graphql::isSameEstate("https://ck.prod.example.test",
                                    "https://elsewhere.invalid")) {
    return 1;
  }

  std::vector<crowdy::graphql::GraphQLErrorDetail> errors(1);
  errors[0].code = std::string(crowdy::graphql::kWrongDatacenterCode);
  errors[0].applicationApiUrl = "https://ck-or.prod.example.test";
  const auto move = crowdy::graphql::moveFromErrors(errors);
  if (!move || move->applicationApiUrl != "https://ck-or.prod.example.test") return 1;

  // A detail built with only the deprecated member still moves (until January 1, 2027).
  std::vector<crowdy::graphql::GraphQLErrorDetail> old(1);
  old[0].code = std::string(crowdy::graphql::kWrongDatacenterCode);
  old[0].gameApiUrl = "https://ck-va.prod.example.test";
  const auto oldMove = crowdy::graphql::moveFromErrors(old);
  if (!oldMove || oldMove->applicationApiUrl != "https://ck-va.prod.example.test") return 1;

  crowdy::graphql::RediscoverCoordinator coordinator;
  if (coordinator.hasCallback()) return 1;

  return 0;
}
