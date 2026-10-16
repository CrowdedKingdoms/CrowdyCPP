#include <memory>
#include <string>
#include <vector>

#include "crowdy/domains/users.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

// updateGamertag must send both halves of UpdateGamertagInput. Through 0.66.0 it
// sent only the gamertag, and every tier refused the variables before the mutation ran.

namespace {

class RecordingHttp final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    return {200,
            R"({"data":{"updateGamertag":{"userId":"1","gamertag":"Ada","disambiguation":"42","userType":"PLAYER"}}})"};
  }
};

std::shared_ptr<graphql::GraphQLClient> clientOn(std::shared_ptr<RecordingHttp> http) {
  return std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, std::move(http),
      std::make_shared<graphql::AuthState>());
}

void expectInput(const graphql::HttpRequest& request, std::string_view gamertag,
                 std::string_view disambiguation) {
  const graphql::Json body = graphql::Json::parse(request.body);
  CHECK(body["query"].asString().find("mutation UpdateGamertag") != std::string::npos);
  const graphql::Json input = body["variables"]["input"];
  CHECK_EQ(input["gamertag"].asString(), std::string(gamertag));
  // An absent key parses as null. String! still requires the field when it is empty.
  CHECK(input["disambiguation"].isString());
  CHECK_EQ(input["disambiguation"].asString(), std::string(disambiguation));
}

void testSyncSendsBothFields() {
  auto http = std::make_shared<RecordingHttp>();
  domains::UsersAPI users(clientOn(http));
  const graphql::Json user = users.updateGamertag("Ada", "42");
  CHECK_EQ(http->requests.size(), std::size_t{1});
  expectInput(http->requests[0], "Ada", "42");
  CHECK_EQ(user["userId"].asString(), std::string("1"));
  CHECK_EQ(user["gamertag"].asString(), std::string("Ada"));
  CHECK_EQ(user["disambiguation"].asString(), std::string("42"));
}

void testEmptyDisambiguationIsSent() {
  auto http = std::make_shared<RecordingHttp>();
  domains::UsersAPI users(clientOn(http));
  users.updateGamertag("Ada", "");
  CHECK_EQ(http->requests.size(), std::size_t{1});
  expectInput(http->requests[0], "Ada", "");
}

void testAsyncSendsBothFields() {
  auto http = std::make_shared<RecordingHttp>();
  domains::UsersAPI users(clientOn(http));
  bool called = false;
  users.updateGamertagAsync("Ada", "42", [&](graphql::GraphQLOutcome out) {
    called = true;
    CHECK(out.ok());
    CHECK_EQ(out.data["gamertag"].asString(), std::string("Ada"));
    CHECK_EQ(out.data["disambiguation"].asString(), std::string("42"));
  });
  CHECK(called);
  CHECK_EQ(http->requests.size(), std::size_t{1});
  expectInput(http->requests[0], "Ada", "42");
}

}  // namespace

int main() {
  testSyncSendsBothFields();
  testEmptyDisambiguationIsSent();
  testAsyncSendsBothFields();
  std::printf("users_test passed\n");
  return 0;
}
