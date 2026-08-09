#include "crowdy/graphql/graphql_client.hpp"

#include <atomic>
#include <string>
#include <utility>

namespace crowdy::graphql {

namespace {

/// Turn a completed HTTP response into an outcome, without throwing. This is
/// the single source of truth for the GraphQL response contract; both the
/// blocking request() and the async path run through it, so their behavior
/// can never drift. The decision order matches the historical request().
GraphQLOutcome interpret(int httpStatus, const std::string& body) {
  GraphQLOutcome out;
  out.httpStatus = httpStatus;

  // GraphQL servers return errors with 200 or 4xx; try to parse either way.
  Json parsed = Json::parse(body);
  if (!parsed.ok()) {
    if (httpStatus < 200 || httpStatus >= 300) {
      out.status = Errc::Rejected;
      out.kind = GraphQLErrorKind::Http;
      out.body = body;
    } else {
      out.status = Errc::Malformed;
      out.kind = GraphQLErrorKind::Protocol;
      out.errorMessage = "GraphQL response is not valid JSON";
    }
    return out;
  }

  Json errors = parsed["errors"];
  if (errors.isArray() && errors.size() > 0) {
    errors.forEach([&](Json e) {
      GraphQLErrorDetail d;
      d.message = e["message"].asString("GraphQL error");
      d.code = e["extensions"]["code"].asString();
      d.remediation = e["extensions"]["remediation"].asString();
      const Json extensions = e["extensions"];
      d.appId = extensions["appId"].asString();
      d.appDatacenter = extensions["appDatacenter"].asString();
      d.servedBy = extensions["servedBy"].asString();
      d.gameApiUrl = extensions["gameApiUrl"].asString();
      d.gameApiWsUrl = extensions["gameApiWsUrl"].asString();
      d.retryable = !extensions["retryable"].isBool() ||
                    extensions["retryable"].asBool();
      Json path = e["path"];
      if (path.isArray()) {
        path.forEach([&](Json seg) {
          if (!d.path.empty()) d.path += '.';
          d.path += seg.isString() ? seg.asString() : std::to_string(seg.asInt64());
        });
      }
      out.errors.push_back(std::move(d));
    });
    out.status = Errc::Rejected;
    out.kind = GraphQLErrorKind::GraphQL;
    return out;
  }

  if (httpStatus < 200 || httpStatus >= 300) {
    out.status = Errc::Rejected;
    out.kind = GraphQLErrorKind::Http;
    out.body = body;
    return out;
  }

  Json data = parsed["data"];
  if (!data.ok()) {
    out.status = Errc::Malformed;
    out.kind = GraphQLErrorKind::Protocol;
    out.errorMessage = "GraphQL response has no data";
    return out;
  }

  out.data = data;
  out.status = Errc::Ok;
  return out;
}

GraphQLOutcome outcomeFromHttp(HttpOutcome http) {
  if (!http.status.ok()) {
    GraphQLOutcome out;
    out.status = http.status;
    out.kind =
        http.status.code == Errc::Timeout ? GraphQLErrorKind::Timeout : GraphQLErrorKind::Network;
    out.errorMessage = std::move(http.errorMessage);
    return out;
  }
  return interpret(http.response.status, http.response.body);
}

#ifndef CROWDY_NO_EXCEPTIONS
[[noreturn]] void throwOutcome(const GraphQLOutcome& out) {
  switch (out.kind) {
    case GraphQLErrorKind::Http: throw CrowdyHttpError(out.httpStatus, out.body);
    case GraphQLErrorKind::GraphQL:
      // Typed, so a caller can tell "this app is down" from any other rejection
      // and show the server's own message instead of retrying blindly.
      if (isAppUnavailable(out.errors)) {
        throw CrowdyAppUnavailableError(out.errors);
      }
      throw CrowdyGraphQLError(out.errors);
    case GraphQLErrorKind::Protocol: throw CrowdyProtocolError(out.errorMessage);
    case GraphQLErrorKind::Network: throw CrowdyNetworkError(out.errorMessage);
    case GraphQLErrorKind::Timeout: throw CrowdyTimeoutError(out.errorMessage);
    case GraphQLErrorKind::None: break;
  }
  throw CrowdyProtocolError("unknown GraphQL failure");
}
#endif

}  // namespace

bool GraphQLClient::applyDatacenterRedirect(const GraphQLOutcome& outcome) {
  if (outcome.kind != GraphQLErrorKind::GraphQL) return false;
  // Checked FIRST: APP_UNAVAILABLE carries no endpoint on purpose, and must not
  // be read as a redirect whose target happens to be missing.
  if (isAppUnavailable(outcome.errors)) return false;
  const auto move = moveFromErrors(outcome.errors);
  if (!move) return false;

  std::function<bool(const DatacenterMove&)> handler;
  {
    std::lock_guard lock(endpointMutex_);
    handler = wrongDatacenterHandler_;
  }
  if (!handler) return false;

#ifndef CROWDY_NO_EXCEPTIONS
  // A handler that throws must not turn a server error into a client crash;
  // the caller still needs the original rejection.
  try {
    return handler(*move);
  } catch (...) {
    return false;
  }
#else
  return handler(*move);
#endif
}

HttpRequest GraphQLClient::buildHttpRequest(std::string_view document, const JVal& variables,
                                            std::string_view operationName) const {
  JVal body;
  body["query"] = JVal(document);
  if (!variables.isNull()) body["variables"] = variables;
  if (!operationName.empty()) body["operationName"] = JVal(operationName);

  HttpRequest req;
  req.url = endpoint();
  req.body = body.dump();
  req.timeoutMs = config_.timeoutMs;
  req.headers.emplace_back("Content-Type", "application/json");
  req.headers.emplace_back("Accept", "application/json");
  const std::string token = auth_->token();
  if (!token.empty()) req.headers.emplace_back("Authorization", "Bearer " + token);
  return req;
}

HttpOutcome GraphQLClient::sendInline(const HttpRequest& request) {
  if (!transport_) {
    HttpOutcome out;
    out.status = Errc::NotConnected;
    out.errorMessage =
        "No default HTTP transport is available; inject IHttpTransport";
    return out;
  }
  return transport_->sendOutcome(request);
}

#ifndef CROWDY_NO_EXCEPTIONS
Json GraphQLClient::request(std::string_view document, const JVal& variables,
                            std::string_view operationName) {
  if (!transport_) {
    throw CrowdyNetworkError(
        "No default HTTP transport is available; inject IHttpTransport");
  }
  HttpRequest req = buildHttpRequest(document, variables, operationName);
  GraphQLOutcome out = outcomeFromHttp(sendInline(req));
  // Exactly one retry, and only after the endpoint actually moved. Retrying a
  // second WRONG_DATACENTER is how two datacenters that disagree about an app
  // turn one query into an infinite ping-pong.
  if (!out.ok() && applyDatacenterRedirect(out)) {
    req = buildHttpRequest(document, variables, operationName);
    out = outcomeFromHttp(sendInline(req));
  }
  if (!out.ok()) throwOutcome(out);
  return out.data;
}
#else
Json GraphQLClient::request(std::string_view document, const JVal& variables,
                            std::string_view operationName) {
  HttpRequest request = buildHttpRequest(document, variables, operationName);
  GraphQLOutcome outcome = outcomeFromHttp(sendInline(request));
  if (!outcome.ok() && applyDatacenterRedirect(outcome)) {
    request = buildHttpRequest(document, variables, operationName);
    outcome = outcomeFromHttp(sendInline(request));
  }
  return outcome.ok() ? outcome.data : Json{};
}
#endif

void GraphQLClient::requestAsync(std::string_view document, const JVal& variables,
                                 std::string_view operationName, GraphQLCallback cb) {
  requestAsyncAttempt(std::string(document), variables,
                      std::string(operationName), std::move(cb), false);
}

void GraphQLClient::requestAsyncAttempt(std::string document,
                                        const JVal& variables,
                                        std::string operationName,
                                        GraphQLCallback cb, bool isRetry) {
  HttpRequest req = buildHttpRequest(document, variables, operationName);

  // Wrap the caller's callback so a redirect is applied and the request
  // re-issued ONCE, before the caller ever sees the failure. `isRetry` is what
  // makes it once: without it, two datacenters disagreeing about an app would
  // bounce a request between them forever, asynchronously and invisibly.
  if (!isRetry) {
    cb = [this, document, variables, operationName,
          cb = std::move(cb)](GraphQLOutcome out) mutable {
      if (!out.ok() && applyDatacenterRedirect(out)) {
        requestAsyncAttempt(document, variables, operationName, std::move(cb),
                            true);
        return;
      }
      cb(std::move(out));
    };
  }

  auto dispatcher = dispatcher_;
  auto scope = asyncScope_;
  auto completed = std::make_shared<std::atomic_bool>(false);
  auto deliver = [dispatcher, scope, completed, cb = std::move(cb)](
                     GraphQLOutcome out) mutable {
    if (completed->exchange(true, std::memory_order_acq_rel)) return;
    auto invoke = [scope, cb, out = std::move(out)]() mutable {
      scope->run([&] { cb(std::move(out)); });
    };
    if (dispatcher) {
      dispatcher->post(std::move(invoke));
    } else {
      invoke();
    }
  };

  if (asyncTransport_) {
    auto complete = [deliver](HttpOutcome http) mutable {
      deliver(outcomeFromHttp(std::move(http)));
    };
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      asyncTransport_->sendAsync(req, std::move(complete));
    } catch (const CrowdyTimeoutError& error) {
      deliver(GraphQLOutcome{Errc::Timeout, GraphQLErrorKind::Timeout, {},
                             {}, 0, error.what(), {}});
    } catch (const std::exception& error) {
      deliver(GraphQLOutcome{Errc::SocketError,
                             GraphQLErrorKind::Network, {}, {}, 0,
                             error.what(), {}});
    } catch (...) {
      deliver(GraphQLOutcome{
          Errc::SocketError, GraphQLErrorKind::Network, {}, {}, 0,
          "Async HTTP transport failed with an unknown exception", {}});
    }
#else
    asyncTransport_->sendAsync(req, std::move(complete));
#endif
  } else {
    deliver(outcomeFromHttp(sendInline(req)));
  }
}

}  // namespace crowdy::graphql
