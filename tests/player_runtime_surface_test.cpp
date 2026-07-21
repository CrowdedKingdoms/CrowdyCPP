#include <cstdio>
#include <memory>
#include <string>
#include <utility>

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

void testGridOwnershipAndPlayerComputeUseGamePlane() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.managementUrl = "https://management.invalid";
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
  input["target"] = "SERVER";
  input["sourceFilesJson"] = "{}";
  transport->response = {200, R"({"data":{"playerComputeDeploy":{"versionId":"v-1"}}})"};
  auto version = client.playerCompute().deploy(input);
  CHECK(version["versionId"].asString() == "v-1");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerComputeDeploy") != std::string::npos);
  CHECK(transport->last.body.find(R"("name":"weather")") != std::string::npos);

  transport->response = {
      200,
      R"({"data":{"playerComputeInvoke":{"resultBase64":"","resultJson":"{}","fuelUsed":"12","durationUs":34}}})"};
  auto invoked =
      client.playerCompute().invoke("1", "2", "weather", "status", "{}");
  CHECK(invoked["resultJson"].asString() == "{}");
  CHECK(transport->last.body.find("PlayerComputeInvoke") != std::string::npos);
}

void testCodeAdmissionsUseManagementPlane() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.managementUrl = "https://management.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {200, R"({"data":{"appCodeAdmissionMode":"IMPLICIT_ALLOW"}})"};
  auto mode = client.admin().apps().codeAdmissionMode("1");
  CHECK(mode.asString() == "IMPLICIT_ALLOW");
  CHECK(transport->last.url == "https://management.invalid/graphql");
  CHECK(transport->last.body.find("AppCodeAdmissionMode") != std::string::npos);

  transport->response = {200, R"({"data":{"setAppCodeAdmissionMode":"ALLOW_LIST"}})"};
  auto updated = client.admin().apps().setCodeAdmissionMode("1", "ALLOW_LIST");
  CHECK(updated.asString() == "ALLOW_LIST");
  CHECK(transport->last.body.find(R"("mode":"ALLOW_LIST")") != std::string::npos);
}

void testPlayerModelAndAutomationsUseGamePlane() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.managementUrl = "https://management.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"playerModelContainers":[{"containerId":"c-1"}]}})"};
  auto containers = client.playerModel().containers("1", "2");
  CHECK(containers.isArray());
  CHECK(containers.at(0)["containerId"].asString() == "c-1");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerModelContainers") !=
        std::string::npos);

  graphql::JVal input;
  input["appId"] = "1";
  input["gridId"] = "2";
  input["name"] = "harvest";
  input["triggerJson"] =
      R"({"kind":"schedule","scheduleKind":"interval","intervalMs":2000})";
  input["actionJson"] =
      R"({"kind":"studio_model_invoke","functionName":"grow","selfContainerId":"c-1"})";
  transport->response = {
      200,
      R"({"data":{"playerAutomationCreate":{"automationId":"a-1"}}})"};
  auto automation = client.playerModel().createAutomation(input);
  CHECK(automation["automationId"].asString() == "a-1");
  CHECK(transport->last.body.find("PlayerAutomationCreate") !=
        std::string::npos);
}

void testPlayerUsageAndSwitchesUseGamePlane() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.managementUrl = "https://management.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"playerComputeUsage":{"hourUnitsUsed":"120","gateStatus":"active","gateReason":null}}})"};
  auto usage = client.playerCompute().usage("1");
  CHECK(usage["hourUnitsUsed"].asString() == "120");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerComputeUsage") != std::string::npos);

  transport->response = {
      200, R"({"data":{"playerComputeRuns":[{"runId":"r-1","success":true}]}})"};
  auto runs = client.playerCompute().runs("1", "2");
  CHECK(runs.isArray());
  CHECK(runs.at(0)["runId"].asString() == "r-1");
  CHECK(transport->last.body.find("PlayerComputeRuns") != std::string::npos);

  graphql::JVal opts;
  opts["scopeRef"] = "7";
  opts["reason"] = "drill";
  transport->response = {200, R"({"data":{"playerComputeSetSwitch":true}})"};
  auto thrown = client.playerCompute().setSwitch("1", "player", true, opts);
  CHECK(thrown.asBool());
  CHECK(transport->last.body.find("PlayerComputeSetSwitch") !=
        std::string::npos);
  CHECK(transport->last.body.find(R"("scope":"player")") != std::string::npos);

  transport->response = {
      200,
      R"({"data":{"playerComputeArtifact":{"versionId":"v-1","artifactHash":"h","sizeBytes":42,"clientFuelPerDispatch":"100000000"}}})"};
  auto artifact = client.playerCompute().artifact("1", "2", "hud");
  CHECK(artifact["versionId"].asString() == "v-1");
  CHECK(transport->last.url == "https://game.invalid/graphql");
  CHECK(transport->last.body.find("PlayerComputeArtifact") !=
        std::string::npos);
}

void testPlayerWalletUsesManagementPlane() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.managementUrl = "https://management.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"playerWalletBalance":{"walletId":"1","balanceCents":"500","currency":"usd"}}})"};
  auto wallet = client.playerWallet().balance();
  CHECK(wallet["balanceCents"].asString() == "500");
  CHECK(transport->last.url == "https://management.invalid/graphql");
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

}  // namespace

int main() {
  testGridOwnershipAndPlayerComputeUseGamePlane();
  testCodeAdmissionsUseManagementPlane();
  testPlayerModelAndAutomationsUseGamePlane();
  testPlayerUsageAndSwitchesUseGamePlane();
  testPlayerWalletUsesManagementPlane();
  std::printf("player_runtime_surface_test passed\n");
  return 0;
}
