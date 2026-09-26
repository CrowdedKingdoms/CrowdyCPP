#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/domains/admin.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

class CaptureTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse response;
  graphql::HttpRequest last;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    last = request;
    return response;
  }
};

void testGridOwnershipAndPlayerComputeUseTheOneOrigin() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {200, R"({"data":{"gridOwnership":{"gridOwnershipId":"o-1"}}})"};
  auto ownership = client.gameApps().ownership("1", "2");
  CHECK(ownership["gridOwnershipId"].asString() == "o-1");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("GridOwnership") != std::string::npos);
  CHECK(transport->last.body.find(R"("gridId":"2")") != std::string::npos);

  graphql::JVal input;
  input["appId"] = "1";
  input["gridId"] = "2";
  input["name"] = "weather";
  input["projectId"] = "11111111-1111-4111-8111-111111111111";
  transport->response = {200, R"({"data":{"playerComputeDeploy":{"versionId":"v-1"}}})"};
  auto version = client.playerCompute().deploy(input);
  CHECK(version["versionId"].asString() == "v-1");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerComputeDeploy") != std::string::npos);
  CHECK(transport->last.body.find(R"("name":"weather")") != std::string::npos);
  // playerCompute().deploy() is the CLIENT target's.
  CHECK(transport->last.body.find(R"("target":"CLIENT")") != std::string::npos);
}

void testCodeAdmissionsUseTheOneOrigin() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {200, R"({"data":{"appCodeAdmissionMode":"IMPLICIT_ALLOW"}})"};
  auto mode = client.admin().apps().codeAdmissionMode("1");
  CHECK(mode.asString() == "IMPLICIT_ALLOW");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("AppCodeAdmissionMode") != std::string::npos);

  transport->response = {200, R"({"data":{"setAppCodeAdmissionMode":"ALLOW_LIST"}})"};
  auto updated = client.admin().apps().setCodeAdmissionMode("1", "ALLOW_LIST");
  CHECK(updated.asString() == "ALLOW_LIST");
  CHECK(transport->last.body.find(R"("mode":"ALLOW_LIST")") != std::string::npos);
}

void testPlayerUsageAndSwitchesUseTheOneOrigin() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"playerComputeUsage":{"hourUnitsUsed":"120","gateStatus":"active","gateReason":null}}})"};
  auto usage = client.playerCompute().usage("1");
  CHECK(usage["hourUnitsUsed"].asString() == "120");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerComputeUsage") != std::string::npos);

  graphql::JVal opts;
  opts["scopeRef"] = "7";
  opts["reason"] = "drill";
  opts["listingRef"] = "listing-1";
  transport->response = {200, R"({"data":{"playerComputeSetSwitch":true}})"};
  auto thrown = client.playerCompute().setSwitch("1", "listing", true, opts);
  CHECK(thrown.asBool());
  CHECK(transport->last.body.find("PlayerComputeSetSwitch") !=
        std::string::npos);
  CHECK(transport->last.body.find(R"("scope":"listing")") != std::string::npos);
  CHECK(transport->last.body.find(R"("listingRef":"listing-1")") !=
        std::string::npos);

  transport->response = {
      200,
      R"({"data":{"playerComputeArtifact":{"versionId":"v-1","artifactHash":"h","sizeBytes":42,"clientFuelPerDispatch":"100000000"}}})"};
  auto artifact = client.playerCompute().artifact("1", "2", "hud");
  CHECK(artifact["versionId"].asString() == "v-1");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerComputeArtifact") !=
        std::string::npos);

  transport->response = {
      200,
      R"({"data":{"playerComputeArtifact":{"versionId":"v-2","artifactHash":"sha256:test","artifactBase64":"AQID","sizeBytes":3,"abiVersion":"1","contractJson":"{}","clientFuelPerDispatch":"9007199254740993"}}})"};
  const auto decoded =
      client.playerCompute().artifactBytes("1", "2", "hud", "v-2");
  CHECK_EQ(decoded.bytes.size(), std::size_t{3});
  CHECK(decoded.bytes[0] == 1 && decoded.bytes[1] == 2 &&
        decoded.bytes[2] == 3);
  CHECK_EQ(decoded.artifactHash, "sha256:test");
  CHECK_EQ(decoded.fuelPerDispatch, "9007199254740993");
  CHECK(decoded.contractJson && *decoded.contractJson == "{}");

  bool decodedAsync = false;
  client.playerCompute().artifactBytesAsync(
      "1", "2", "hud", "v-2",
      [&](graphql::GraphQLOutcome outcome,
          domains::ClientArtifactBytes value) {
        decodedAsync = true;
        CHECK(outcome.ok());
        CHECK_EQ(value.bytes.size(), std::size_t{3});
        CHECK_EQ(value.versionId, "v-2");
      });
  CHECK(!decodedAsync);
  client.poll();
  CHECK(decodedAsync);
}

void testAuthPortalParityConveniences() {
  const auto auth = domains::AuthResponse::fromJson(graphql::Json::parse(
      R"({"token":"session","gameTokenId":"9007199254740993","user":{"userId":"7","email":"player@example.com","gamertag":"player"}})"));
  CHECK_EQ(auth.gameTokenId, "9007199254740993");

  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"authorizeApp":{"grantId":"grant-1","appId":"2","status":"ACTIVE","scopes":["profile","play"]}}})"};
  const auto grant = client.portal().authorizeApp(
      "2", std::vector<std::string>{"profile", "play"});
  CHECK_EQ(grant["grantId"].asString(), "grant-1");
  CHECK_EQ(transport->last.url, "https://game.invalid/graphql");
  CHECK(transport->last.body.find(
            R"("scopes":["profile","play"])") != std::string::npos);

}

void testPlayerWalletUsesTheOneOrigin() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"playerWalletBalance":{"walletId":"1","balanceCents":"500","currency":"usd"}}})"};
  auto wallet = client.playerWallet().balance();
  CHECK(wallet["balanceCents"].asString() == "500");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerWalletBalance") != std::string::npos);

  graphql::JVal caps;
  caps["appId"] = "1";
  caps["dailyLimitCents"] = "100";
  transport->response = {
      200, R"({"data":{"setPlayerSpendCap":[{"scope":"app","dailyLimitCents":"100"}]}})"};
  auto updated = client.playerWallet().setSpendCap("app", caps);
  CHECK(updated.isArray());
  CHECK(transport->last.body.find("SetPlayerSpendCap") != std::string::npos);

  graphql::JVal policy;
  policy["appId"] = "1";
  policy["scope"] = "app_default";
  policy["unitsPerHour"] = "1000";
  transport->response = {
      200,
      R"({"data":{"setPlayerWasmPolicy":{"policyId":"p-1","unitsPerHour":"1000"}}})"};
  auto set = client.playerWallet().setPolicy(policy);
  CHECK(set["policyId"].asString() == "p-1");
  CHECK(transport->last.body.find("SetPlayerWasmPolicy") != std::string::npos);

  transport->response = {200, R"({"data":{"setPlayerRateMarkup":2000}})"};
  auto markup = client.playerWallet().setRateMarkup("1", 2000);
  CHECK(markup.asInt64() == 2000);
  CHECK(transport->last.body.find(R"("markupBps":2000)") != std::string::npos);
}

void testMarketplaceChunkClaimsUseAppToken() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));
  const std::string appToken(64, 'a');
  client.setToken(appToken);

  transport->response = {
      200,
      R"({"data":{"claimGridChunk":{"gridId":"42","lowChunk":{"x":"-2","y":"3","z":"7"},"highChunk":{"x":"-2","y":"3","z":"7"},"policy":"SELF_CLAIM","ownership":{"gridOwnershipId":"ownership-42","ownerKind":"USER","ownerRef":"7","tenure":"OWNED","acquiredVia":"self_claim_chunk","acquiredAt":"2026-07-22T00:00:00.000Z","expiresAt":null},"moddable":true,"effectivePermissionKeys":["access","update_voxel_data","write_server_code","run_server_code"]}}})"};
  graphql::Json claimed =
      client.marketplace().claimGridChunk("2", domains::ChunkRef{-2, 3, 7});
  CHECK_EQ(claimed["gridId"].asString(), "42");
  CHECK_EQ(claimed["lowChunk"]["x"].asString(), "-2");
  CHECK_EQ(claimed["ownership"]["ownerRef"].asString(), "7");
  CHECK(claimed["moddable"].asBool());
  CHECK_EQ(transport->last.url, "https://game.invalid/graphql");
  CHECK(transport->last.body.find("MarketplaceClaimGridChunk") !=
        std::string::npos);
  CHECK(transport->last.body.find(
            R"("appId":"2","chunk":{"x":"-2","y":"3","z":"7"})") !=
        std::string::npos);
  bool bearerFound = false;
  for (const auto& [name, value] : transport->last.headers) {
    if (name == "Authorization" && value == "Bearer " + appToken) {
      bearerFound = true;
    }
  }
  CHECK(bearerFound);

  bool claimAsyncCalled = false;
  client.marketplace().claimGridChunkAsync(
      "2", domains::ChunkRef{-2, 3, 7},
      [&](graphql::GraphQLOutcome out) {
        claimAsyncCalled = true;
        CHECK(out.ok());
        CHECK_EQ(out.data["gridId"].asString(), "42");
      });
  CHECK(!claimAsyncCalled);
  client.poll();
  CHECK(claimAsyncCalled);

  transport->response = {
      200,
      R"({"data":{"releaseClaimedGrid":{"gridId":"42","lowChunk":{"x":"-2","y":"3","z":"7"},"highChunk":{"x":"-2","y":"3","z":"7"},"policy":"SELF_CLAIM","released":true}}})"};
  graphql::Json released =
      client.marketplace().releaseClaimedGrid("2", "42");
  CHECK_EQ(released["gridId"].asString(), "42");
  CHECK(released["released"].asBool());
  CHECK(transport->last.body.find("MarketplaceReleaseClaimedGrid") !=
        std::string::npos);
  CHECK(transport->last.body.find(R"("appId":"2","gridId":"42")") !=
        std::string::npos);

  bool releaseAsyncCalled = false;
  client.marketplace().releaseClaimedGridAsync(
      "2", "42", [&](graphql::GraphQLOutcome out) {
        releaseAsyncCalled = true;
        CHECK(out.ok());
        CHECK(out.data["released"].asBool());
      });
  CHECK(!releaseAsyncCalled);
  client.poll();
  CHECK(releaseAsyncCalled);
}

void testMarketplaceAdminAdditions() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"appPlayerCodeListingVersions":[{"versionId":"version-1","listingId":"listing-1","appId":"2","versionNo":1,"serverArtifactHashes":[],"clientArtifactHashes":[],"requirements":[],"capabilitySummaryJson":"{}","capabilityHash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","openSource":false,"licenseText":null,"createdAt":"2026-07-24T00:00:00.000Z"}]}})"};
  graphql::JVal vars;
  vars["appId"] = "2";
  vars["listingId"] = "listing-1";
  const auto versions = client.marketplace().appListingVersions(vars);
  CHECK_EQ(versions.size(), std::size_t{1});
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("MarketplaceAppListingVersions") !=
        std::string::npos);

  bool versionsAsync = false;
  client.marketplace().appListingVersionsAsync(
      vars, [&](graphql::GraphQLOutcome outcome) {
        versionsAsync = true;
        CHECK(outcome.ok());
        CHECK_EQ(outcome.data.size(), std::size_t{1});
      });
  CHECK(!versionsAsync);
  client.poll();
  CHECK(versionsAsync);
}

}  // namespace

int main() {
  testGridOwnershipAndPlayerComputeUseTheOneOrigin();
  testCodeAdmissionsUseTheOneOrigin();
  testPlayerUsageAndSwitchesUseTheOneOrigin();
  testAuthPortalParityConveniences();
  testPlayerWalletUsesTheOneOrigin();
  testMarketplaceChunkClaimsUseAppToken();
  testMarketplaceAdminAdditions();
  std::printf("player_runtime_surface_test passed\n");
  return 0;
}
