#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "crowdy/graphql/json.hpp"

/// Typed results for the hot game-client paths. Long-tail / studio-admin
/// results are returned as graphql::Json documents; every field the server
/// sends is reachable.
///
/// GraphQL BigInt scalars (ids, chunk coordinates) cross the wire as decimal
/// strings — mirrored here as std::string to avoid silent precision bugs,
/// with int64 accessors where useful.
namespace crowdy::domains {

/// Chunk address used by GraphQL world APIs (BigInt strings on the wire).
struct ChunkRef {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;

  graphql::JVal toInput() const {
    graphql::JVal v;
    v["x"] = std::to_string(x);
    v["y"] = std::to_string(y);
    v["z"] = std::to_string(z);
    return v;
  }
};

/// Result of portal.mintAppToken / exchangeCode / refresh.
struct AppTokenResponse {
  std::string token;        ///< 64-char app-scoped token (also the UDP HMAC key)
  std::int64_t gameTokenId = 0;
  std::string appId;
  std::string expiresAt;    ///< ISO-8601; empty when non-expiring
  std::string gameApiUrl;   ///< app's Game API endpoint when routed
  std::string gameApiWsUrl;
  std::string launchUrl;

  static AppTokenResponse fromJson(const graphql::Json& j) {
    AppTokenResponse r;
    r.token = j["token"].asString();
    r.gameTokenId = j["gameTokenId"].asBigInt();
    r.appId = j["appId"].asString();
    r.expiresAt = j["expiresAt"].asString();
    r.gameApiUrl = j["gameApiUrl"].asString();
    r.gameApiWsUrl = j["gameApiWsUrl"].asString();
    r.launchUrl = j["launchUrl"].asString();
    return r;
  }
};

/// Result of the passwordless sign-in mutations.
struct AuthResponse {
  std::string token;  ///< identity SESSION token (management-plane only)
  std::string userId;
  std::string email;
  std::string gamertag;

  static AuthResponse fromJson(const graphql::Json& j) {
    AuthResponse r;
    r.token = j["token"].asString();
    r.userId = j["user"]["userId"].asString();
    r.email = j["user"]["email"].asString();
    r.gamertag = j["user"]["gamertag"].asString();
    return r;
  }
};

/// Result of serverStatus.serverWithLeastClients — the Buddy-server
/// assignment for native UDP (calling it also installs the UDP session
/// server-side).
struct ServerAssignment {
  std::string serverId;
  std::string ip4;
  std::string ip6;
  int clientPort = 0;
  std::string status;
  std::int64_t clients = 0;

  static ServerAssignment fromJson(const graphql::Json& j) {
    ServerAssignment a;
    a.serverId = j["serverId"].asString(j["id"].asString());
    a.ip4 = j["ip4"].asString();
    a.ip6 = j["ip6"].asString();
    a.clientPort = static_cast<int>(j["clientPort"].asInt64());
    a.status = j["status"].asString();
    a.clients = j["clients"].asInt64();
    return a;
  }
};

}  // namespace crowdy::domains
