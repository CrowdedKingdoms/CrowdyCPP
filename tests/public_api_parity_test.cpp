#include <concepts>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/types.hpp"

namespace {

using crowdy::domains::AuthResponse;
using crowdy::domains::ClientArtifactBytes;
using crowdy::domains::PlayerComputeAPI;
using crowdy::domains::PortalAPI;
using crowdy::graphql::GraphQLCallback;
using crowdy::graphql::JVal;
using crowdy::graphql::Json;

static_assert(std::same_as<
              decltype(std::declval<AuthResponse>().gameTokenId),
              std::string>);
static_assert(std::same_as<
              decltype(std::declval<ClientArtifactBytes>().bytes),
              std::vector<std::uint8_t>>);
static_assert(std::same_as<
              decltype(std::declval<ClientArtifactBytes>().fuelPerDispatch),
              std::string>);
static_assert(std::same_as<
              decltype(std::declval<ClientArtifactBytes>().contractJson),
              std::optional<std::string>>);

template <typename API>
concept PlayerComputeParity =
    requires(API& api, std::string_view id, GraphQLCallback callback,
             API::ArtifactBytesCallback artifactCallback) {
      // Three arguments proves the optional version id remains omitted.
      { api.artifactBytes(id, id, id) } ->
          std::same_as<ClientArtifactBytes>;
      { api.artifactBytesAsync(id, id, id, artifactCallback) } ->
          std::same_as<void>;
    };

static_assert(PlayerComputeParity<PlayerComputeAPI>);

using AuthorizeAppSignature = Json (PortalAPI::*)(
    std::string_view,
    std::optional<std::vector<std::string>>) const;
static_assert(std::same_as<
              decltype(static_cast<AuthorizeAppSignature>(
                  &PortalAPI::authorizeApp)),
              AuthorizeAppSignature>);
static_assert(requires(PortalAPI& api, std::string_view appId) {
  // One argument proves omitted scopes still use the server-side baseline.
  { api.authorizeApp(appId) } -> std::same_as<Json>;
});

static_assert(requires(const PortalAPI& api, std::string_view ip4,
                       std::function<void(crowdy::graphql::GraphQLOutcome,
                                          crowdy::domains::AppTokenResponse)> cb) {
  api.refreshAsync(cb);
  api.refreshAsync(cb, false);
  api.refreshAsync(ip4, 39001, cb);
  api.refreshAsync(ip4, 39001, cb, false);
});

}  // namespace

int main() {
  std::printf("public_api_parity_test passed\n");
  return 0;
}
