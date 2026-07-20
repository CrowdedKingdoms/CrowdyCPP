#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/json.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::graphql;

namespace {

// A synchronous transport that returns a canned response, or throws.
class FakeSyncTransport final : public IHttpTransport {
 public:
  HttpResponse response;
  bool throwTimeout = false;
  bool throwNetwork = false;

  HttpResponse send(const HttpRequest&) override {
    if (throwTimeout) throw CrowdyTimeoutError("timed out");
    if (throwNetwork) throw CrowdyNetworkError("connection refused");
    return response;
  }
};

// An async transport that either calls back immediately or holds the callback
// until fire() is called (to test deferred delivery through the dispatcher).
class FakeAsyncTransport final : public IAsyncHttpTransport {
 public:
  HttpOutcome outcome;
  bool defer = false;
  std::function<void(HttpOutcome)> saved;

  void sendAsync(const HttpRequest&, std::function<void(HttpOutcome)> cb) override {
    if (defer) {
      saved = std::move(cb);
    } else {
      cb(outcome);
    }
  }

  void fire() { saved(outcome); }
};

std::shared_ptr<GraphQLClient> makeClient(std::shared_ptr<IHttpTransport> sync) {
  return std::make_shared<GraphQLClient>(GraphQLClientConfig{"http://test/graphql", 1000},
                                         std::move(sync), std::make_shared<AuthState>());
}

HttpOutcome httpOk(int status, std::string body) {
  HttpOutcome o;
  o.status = Errc::Ok;
  o.response = HttpResponse{status, std::move(body)};
  return o;
}

void testAsyncSuccess() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200, R"({"data":{"foo":{"x":7}}})");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  bool called = false;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    got = std::move(out);
    called = true;
  });

  CHECK(called);
  CHECK(got.ok());
  CHECK_EQ(got.data["foo"]["x"].asInt64(), 7);
}

void testAsyncGraphqlErrors() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200, R"({"errors":[{"message":"nope","extensions":{"code":"FORBIDDEN"}}]})");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::GraphQL);
  CHECK_EQ(got.errors.size(), std::size_t{1});
  CHECK(got.errors[0].code == "FORBIDDEN");
  CHECK(got.errors[0].message == "nope");
}

void testAsyncHttpError() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(500, "internal error, not json");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Http);
  CHECK_EQ(got.httpStatus, 500);
}

void testAsyncProtocolError() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200, "this is not json");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Protocol);
}

void testAsyncTransportFailure() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome.status = Errc::Timeout;
  async->outcome.errorMessage = "timed out";
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Timeout);
  CHECK(got.errorMessage == "timed out");
}

// The pump: a completion is not delivered until drain() runs on the poll thread.
void testDispatcherDefersDelivery() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->defer = true;
  async->outcome = httpOk(200, R"({"data":{"ok":true}})");
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);
  client->setDispatcher(dispatcher);

  bool called = false;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome) { called = true; });
  CHECK(!called);  // request started, transport has not completed

  async->fire();
  CHECK(!called);  // transport completed, but delivery is queued on the dispatcher

  const std::size_t ran = dispatcher->drain();
  CHECK(called);
  CHECK_EQ(ran, std::size_t{1});
}

// With no async transport, requestAsync falls back to the sync transport inline.
void testInlineFallback() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{200, R"({"data":{"n":42}})"};
  auto client = makeClient(sync);

  GraphQLOutcome got;
  bool called = false;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    got = std::move(out);
    called = true;
  });

  CHECK(called);
  CHECK(got.ok());
  CHECK_EQ(got.data["n"].asInt64(), 42);
}

void testInlineFallbackTransportThrow() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->throwTimeout = true;
  auto client = makeClient(sync);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Timeout);
}

// The blocking request() still works and still throws, derived from the same
// logic as the async path.
void testSyncRequestReturnsData() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{200, R"({"data":{"v":"hello"}})"};
  auto client = makeClient(sync);

  Json data = client->request("query");
  CHECK(data["v"].asString() == "hello");
}

void testSyncRequestThrowsGraphql() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{200, R"({"errors":[{"message":"denied","extensions":{"code":"UNAUTHENTICATED"}}]})"};
  auto client = makeClient(sync);

  bool threw = false;
  try {
    client->request("query");
  } catch (const CrowdyGraphQLError& e) {
    threw = true;
    CHECK(e.code() == "UNAUTHENTICATED");
  }
  CHECK(threw);
}

void testSyncRequestThrowsHttp() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{503, "unavailable"};
  auto client = makeClient(sync);

  bool threw = false;
  try {
    client->request("query");
  } catch (const CrowdyHttpError& e) {
    threw = true;
    CHECK_EQ(e.status(), 503);
  }
  CHECK(threw);
}

}  // namespace

int main() {
  testAsyncSuccess();
  testAsyncGraphqlErrors();
  testAsyncHttpError();
  testAsyncProtocolError();
  testAsyncTransportFailure();
  testDispatcherDefersDelivery();
  testInlineFallback();
  testInlineFallbackTransportThrow();
  testSyncRequestReturnsData();
  testSyncRequestThrowsGraphql();
  testSyncRequestThrowsHttp();
  std::printf("graphql_client_test passed\n");
  return 0;
}
