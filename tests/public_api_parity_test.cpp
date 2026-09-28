#include <concepts>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "crowdy/domains/exec.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/types.hpp"

namespace {

using crowdy::domains::AuthResponse;
using crowdy::domains::ExecAPI;
using crowdy::domains::ExecModClientArtifactBytes;
using crowdy::domains::ExecModClientArtifactBytesCallback;
using crowdy::domains::PortalAPI;
using crowdy::graphql::GraphQLCallback;
using crowdy::graphql::JVal;
using crowdy::graphql::Json;

static_assert(std::same_as<
              decltype(std::declval<AuthResponse>().gameTokenId),
              std::string>);
static_assert(std::same_as<
              decltype(std::declval<ExecModClientArtifactBytes>().bytes),
              std::vector<std::uint8_t>>);
// GraphQL BigInt fuel stays decimal text so a native engine picks its own width.
static_assert(std::same_as<
              decltype(std::declval<ExecModClientArtifactBytes>().fuelPerDispatch),
              std::string>);
static_assert(std::same_as<
              decltype(std::declval<ExecModClientArtifactBytes>().capabilitySummary.hostFunctions),
              std::vector<std::string>>);

template <typename API>
concept ClientHalfParity =
    requires(const API& api, std::string id, ExecModClientArtifactBytesCallback callback) {
      { api.modClientArtifactBytes(id, id) } -> std::same_as<ExecModClientArtifactBytes>;
      { api.modClientArtifactBytesAsync(id, id, callback) } -> std::same_as<void>;
    };

static_assert(ClientHalfParity<ExecAPI>);

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
