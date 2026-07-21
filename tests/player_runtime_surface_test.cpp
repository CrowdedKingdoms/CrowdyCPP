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

}  // namespace

int main() {
  testGridOwnershipAndPlayerComputeUseGamePlane();
  testCodeAdmissionsUseManagementPlane();
  testPlayerModelAndAutomationsUseGamePlane();
  std::printf("player_runtime_surface_test passed\n");
  return 0;
}
