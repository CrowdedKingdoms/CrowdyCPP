#include <crowdy/crowdy.hpp>

int main() {
  const auto endpoint =
      crowdy::graphql::normalizeGraphQLWebSocketUrl(
          "https://api.example.test");
  return endpoint.ok() &&
                 endpoint.value() == "wss://api.example.test/graphql"
             ? 0
             : 1;
}
