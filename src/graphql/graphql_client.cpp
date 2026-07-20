#include "crowdy/graphql/graphql_client.hpp"

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
    case GraphQLErrorKind::GraphQL: throw CrowdyGraphQLError(out.errors);
    case GraphQLErrorKind::Protocol: throw CrowdyProtocolError(out.errorMessage);
    case GraphQLErrorKind::Network: throw CrowdyNetworkError(out.errorMessage);
    case GraphQLErrorKind::Timeout: throw CrowdyTimeoutError(out.errorMessage);
    case GraphQLErrorKind::None: break;
  }
  throw CrowdyProtocolError("unknown GraphQL failure");
}
#endif

}  // namespace

HttpRequest GraphQLClient::buildHttpRequest(std::string_view document, const JVal& variables,
                                            std::string_view operationName) const {
  JVal body;
  body["query"] = JVal(document);
  if (!variables.isNull()) body["variables"] = variables;
  if (!operationName.empty()) body["operationName"] = JVal(operationName);

  HttpRequest req;
  req.url = config_.endpoint;
  req.body = body.dump();
  req.timeoutMs = config_.timeoutMs;
  req.headers.emplace_back("Content-Type", "application/json");
  req.headers.emplace_back("Accept", "application/json");
  const std::string token = auth_->token();
  if (!token.empty()) req.headers.emplace_back("Authorization", "Bearer " + token);
  return req;
}

HttpOutcome GraphQLClient::sendInline(const HttpRequest& request) {
  HttpOutcome out;
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    out.response = transport_->send(request);
    out.status = Errc::Ok;
  } catch (const CrowdyTimeoutError& e) {
    out.status = Errc::Timeout;
    out.errorMessage = e.what();
  } catch (const std::exception& e) {
    out.status = Errc::SocketError;
    out.errorMessage = e.what();
  }
#else
  out.response = transport_->send(request);
  out.status = Errc::Ok;
#endif
  return out;
}

#ifndef CROWDY_NO_EXCEPTIONS
Json GraphQLClient::request(std::string_view document, const JVal& variables,
                            std::string_view operationName) {
  HttpRequest req = buildHttpRequest(document, variables, operationName);
  HttpResponse res = transport_->send(req);
  GraphQLOutcome out = interpret(res.status, res.body);
  if (!out.ok()) throwOutcome(out);
  return out.data;
}
#endif

void GraphQLClient::requestAsync(std::string_view document, const JVal& variables,
                                 std::string_view operationName, GraphQLCallback cb) {
  HttpRequest req = buildHttpRequest(document, variables, operationName);

  auto dispatcher = dispatcher_;
  auto deliver = [dispatcher, cb = std::move(cb)](GraphQLOutcome out) mutable {
    if (dispatcher) {
      dispatcher->post([cb, out = std::move(out)]() mutable { cb(std::move(out)); });
    } else {
      cb(std::move(out));
    }
  };

  if (asyncTransport_) {
    asyncTransport_->sendAsync(req, [deliver = std::move(deliver)](HttpOutcome http) mutable {
      deliver(outcomeFromHttp(std::move(http)));
    });
  } else {
    deliver(outcomeFromHttp(sendInline(req)));
  }
}

}  // namespace crowdy::graphql
