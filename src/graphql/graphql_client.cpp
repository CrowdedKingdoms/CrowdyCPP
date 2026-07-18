#include "crowdy/graphql/graphql_client.hpp"

namespace crowdy::graphql {

Json GraphQLClient::request(std::string_view document, const JVal& variables,
                            std::string_view operationName) {
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

  HttpResponse res = transport_->send(req);

  // GraphQL servers return errors with 200 or 4xx; try to parse either way.
  Json parsed = Json::parse(res.body);
  if (!parsed.ok()) {
    if (res.status < 200 || res.status >= 300) throw CrowdyHttpError(res.status, res.body);
    throw CrowdyProtocolError("GraphQL response is not valid JSON");
  }

  Json errors = parsed["errors"];
  if (errors.isArray() && errors.size() > 0) {
    std::vector<GraphQLErrorDetail> details;
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
      details.push_back(std::move(d));
    });
    throw CrowdyGraphQLError(std::move(details));
  }

  if (res.status < 200 || res.status >= 300) throw CrowdyHttpError(res.status, res.body);

  Json data = parsed["data"];
  if (!data.ok()) throw CrowdyProtocolError("GraphQL response has no data");
  return data;
}

}  // namespace crowdy::graphql
