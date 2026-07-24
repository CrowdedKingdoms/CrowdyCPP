#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/replication/connection.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

const std::string kOldToken(64, 'o');
const std::string kFreshToken(64, 'f');

std::string bearer(const graphql::HttpRequest& request) {
  for (const auto& [name, value] : request.headers) {
    if (name == "Authorization") return value;
  }
  return {};
}

class PortableTransport final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  int assignmentCalls = 0;
  int refreshCalls = 0;
  bool failRefresh = false;
  bool failReconnect = false;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    if (request.body.find("RefreshAppToken") != std::string::npos) {
      ++refreshCalls;
      if (failRefresh) {
        return {
            200,
            R"({"errors":[{"message":"refresh refused","extensions":{"code":"UNAUTHENTICATED"}}]})"};
      }
      return {
          200,
          std::string(
              R"({"data":{"refreshAppToken":{"token":")") +
              kFreshToken +
              R"(","gameTokenId":"202","appId":"42","expiresAt":"2030-01-01T00:00:00.000Z","gameApiUrl":"https://game.invalid","gameApiWsUrl":"wss://game.invalid","launchUrl":null}}})"};
    }
    if (request.body.find("ServerWithLeastClients") != std::string::npos) {
      ++assignmentCalls;
      if (failReconnect && assignmentCalls > 1) {
        return {
            200,
            R"({"errors":[{"message":"no replication server","extensions":{"code":"UNAVAILABLE"}}]})"};
      }
      return {
          200,
          R"({"data":{"serverWithLeastClients":{"serverId":"server-1","ip4":"127.0.0.1","ip6":"","clientPort":39001,"status":"READY","peers":[],"clients":"0","cpuPeakPct":0,"updatedAt":"","createdAt":""}}})"};
    }
    return {200, R"({"data":{"ok":true}})"};
  }
};

ClientConfig portableConfig(
    const std::shared_ptr<PortableTransport>& transport) {
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.wsUrl = "wss://game.invalid";
  config.managementUrl = "https://management.invalid";
  config.transport = transport;
  return config;
}

std::shared_ptr<replication::Connection> connectNative(
    CrowdyClient& client, int* statusCalls = nullptr) {
  replication::Config config;
  config.appId = 42;
  config.token = {kOldToken, 101, 0};
  config.manualPump = true;
  config.sessionReadyWaitMs = 0;
  replication::Handlers handlers;
  if (statusCalls) {
    handlers.status = [statusCalls](replication::ConnState) {
      ++*statusCalls;
    };
  }
  return client.replication().connect(config, std::move(handlers));
}

void testEndpointNormalization() {
  struct Case {
    std::string http;
    std::string ws;
    std::string expectedHttp;
    std::string expectedWs;
  };
  const std::vector<Case> cases = {
      {"http://game.invalid", "ws://game.invalid",
       "http://game.invalid/graphql", "ws://game.invalid/graphql"},
      {"https://game.invalid/", "wss://game.invalid/",
       "https://game.invalid/graphql", "wss://game.invalid/graphql"},
      {"https://game.invalid/tenant/7///",
       "wss://game.invalid/tenant/7///",
       "https://game.invalid/tenant/7/graphql",
       "wss://game.invalid/tenant/7/graphql"},
      {"https://game.invalid/graphql/",
       "wss://game.invalid/graphql/",
       "https://game.invalid/graphql", "wss://game.invalid/graphql"},
      {" https://game.invalid/graphql/graphql/ ",
       " wss://game.invalid/graphql/graphql/ ",
       "https://game.invalid/graphql", "wss://game.invalid/graphql"},
  };

  for (const auto& testCase : cases) {
    auto transport = std::make_shared<PortableTransport>();
    ClientConfig config;
    config.httpUrl = testCase.http;
    config.wsUrl = testCase.ws;
    config.managementUrl = "https://management.invalid/";
    config.transport = transport;
    CrowdyClient client(std::move(config));
    CHECK_EQ(client.graphqlClient().endpoint(), testCase.expectedHttp);
    CHECK_EQ(client.websocketEndpoint(), testCase.expectedWs);
    CHECK_EQ(client.managementClient().endpoint(),
             "https://management.invalid/graphql");
  }

  auto transport = std::make_shared<PortableTransport>();
  ClientConfig custom;
  custom.httpUrl = "https://game.invalid/base/";
  custom.wsUrl = "wss://game.invalid/base/";
  custom.managementUrl = "https://management.invalid/base/";
  custom.graphqlEndpoint = "https://custom.invalid/game-query/";
  custom.managementGraphqlEndpoint =
      "https://custom.invalid/management-query/";
  custom.wsEndpoint = "wss://custom.invalid/subscriptions/";
  custom.transport = transport;
  CrowdyClient explicitEndpoints(std::move(custom));
  CHECK_EQ(explicitEndpoints.graphqlClient().endpoint(),
           "https://custom.invalid/game-query/");
  CHECK_EQ(explicitEndpoints.managementClient().endpoint(),
           "https://custom.invalid/management-query/");
  CHECK_EQ(explicitEndpoints.websocketEndpoint(),
           "wss://custom.invalid/subscriptions/");

  ClientConfig customPaths;
  customPaths.httpUrl = "https://game.invalid/root/";
  customPaths.wsUrl = "wss://game.invalid/root/";
  customPaths.managementUrl = "https://management.invalid/root/";
  customPaths.graphqlEndpoint = "/v2/query";
  customPaths.managementGraphqlEndpoint = "/identity/query";
  customPaths.wsEndpoint = "/stream";
  customPaths.transport = transport;
  CrowdyClient relativeEndpoints(std::move(customPaths));
  CHECK_EQ(relativeEndpoints.graphqlClient().endpoint(),
           "https://game.invalid/root/v2/query");
  CHECK_EQ(relativeEndpoints.managementClient().endpoint(),
           "https://management.invalid/root/identity/query");
  CHECK_EQ(relativeEndpoints.websocketEndpoint(),
           "wss://game.invalid/root/stream");
}

void testSynchronousGameplayRefresh() {
  auto transport = std::make_shared<PortableTransport>();
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  int statusCalls = 0;
  auto connection = connectNative(client, &statusCalls);
  CHECK_EQ(transport->assignmentCalls, 1);

  GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(result.ok());
  CHECK(result.tokenInstalled);
  CHECK(result.reconnectAttempted);
  CHECK(result.reconnected);
  CHECK(result.hadActiveReplication);
  CHECK_EQ(result.previousReplicationState,
           replication::ConnState::Connecting);
  CHECK_EQ(result.previousReplicationEndpoint.ip4, "127.0.0.1");
  CHECK_EQ(result.previousReplicationEndpoint.clientPort, 39001);
  CHECK_EQ(result.replicationEndpoint.ip4, "127.0.0.1");
  CHECK_EQ(client.getToken(), kFreshToken);
  CHECK_EQ(result.token.token, kFreshToken);
  CHECK_EQ(transport->assignmentCalls, 2);
  CHECK_EQ(transport->refreshCalls, 1);
  CHECK(client.replication().activeConnection() == connection);
  CHECK(static_cast<bool>(connection->snapshot().handlers.status));

  CHECK_EQ(bearer(transport->requests.at(0)), "Bearer " + kOldToken);
  CHECK_EQ(bearer(transport->requests.at(1)), "Bearer " + kOldToken);
  CHECK_EQ(bearer(transport->requests.at(2)), "Bearer " + kFreshToken);

  connection->pump();
  connection->poll();
  CHECK(statusCalls > 0);
}

void testRefreshFailureRetainsOldToken() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failRefresh = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Refresh);
  CHECK_EQ(result.errorCode, "UNAUTHENTICATED");
  CHECK_EQ(client.getToken(), kOldToken);
  CHECK(!result.tokenInstalled);
  CHECK(!result.reconnectAttempted);
  CHECK_EQ(connection->state(), replication::ConnState::Closed);

  transport->failRefresh = false;
  CHECK(connection->connect().ok());
  CHECK_EQ(bearer(transport->requests.back()), "Bearer " + kOldToken);
}

void testReconnectFailureRetainsFreshToken() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failReconnect = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Reconnect);
  CHECK(result.tokenInstalled);
  CHECK(result.reconnectAttempted);
  CHECK(!result.reconnected);
  CHECK_EQ(client.getToken(), kFreshToken);
  CHECK_EQ(connection->state(), replication::ConnState::Failed);

  transport->failReconnect = false;
  CHECK(connection->connect().ok());
  CHECK_EQ(bearer(transport->requests.back()), "Bearer " + kFreshToken);
}

void testAsyncGameplayRefreshUsesPoll() {
  auto transport = std::make_shared<PortableTransport>();
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  bool called = false;
  GameplayTokenRefreshResult result;
  client.refreshGameplayTokenAsync(
      [&](GameplayTokenRefreshResult value) {
        called = true;
        result = std::move(value);
      });
  CHECK(!called);
  CHECK_EQ(connection->state(), replication::ConnState::Closed);
  CHECK_EQ(client.getToken(), kOldToken);

  client.poll();
  CHECK(called);
  CHECK(result.ok());
  CHECK(result.reconnected);
  CHECK_EQ(client.getToken(), kFreshToken);
}

void testAsyncRefreshFailureUsesPollAndRetainsOldToken() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failRefresh = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  bool called = false;
  GameplayTokenRefreshResult result;
  client.refreshGameplayTokenAsync(
      [&](GameplayTokenRefreshResult value) {
        called = true;
        result = std::move(value);
      });
  CHECK(!called);
  CHECK_EQ(client.getToken(), kOldToken);
  client.poll();

  CHECK(called);
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Refresh);
  CHECK_EQ(result.errorCode, "UNAUTHENTICATED");
  CHECK_EQ(client.getToken(), kOldToken);
  CHECK_EQ(connection->state(), replication::ConnState::Closed);
}

}  // namespace

int main() {
  testEndpointNormalization();
  testSynchronousGameplayRefresh();
  testRefreshFailureRetainsOldToken();
  testReconnectFailureRetainsFreshToken();
  testAsyncGameplayRefreshUsesPoll();
  testAsyncRefreshFailureUsesPollAndRetainsOldToken();
  std::puts("client_portable_test OK");
  return 0;
}
