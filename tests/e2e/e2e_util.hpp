#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "crowdy/crowdy.hpp"
#include "crowdy/session/world_session.hpp"

/// Shared harness for the env-gated e2e suites. Configure a deployment with:
///
///   CROWDY_E2E_MANAGEMENT_URL   Management API base URL
///   CROWDY_E2E_HTTP_URL         Game API base URL (optional; falls back to
///                               the gameApiUrl minted with the app token)
///   CROWDY_E2E_EMAIL            sign-in email (server must have DEV_AUTH_BYPASS)
///   CROWDY_E2E_EMAIL_2          second player's email (two-client suites)
///   CROWDY_E2E_APP_ID           app id (decimal string)
///
/// Tests exit 77 (ctest SKIP_RETURN_CODE) when the variables are unset.
namespace e2e {

#define E2E_CHECK(cond)                                                            \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "E2E_CHECK failed: %s at %s:%d\n", #cond, __FILE__,     \
                   __LINE__);                                                      \
      std::exit(1);                                                                \
    }                                                                              \
  } while (0)

inline std::string envOr(const char* name, const char* fallback = "") {
  const char* v = std::getenv(name);
  return v ? v : fallback;
}

struct E2eConfig {
  std::string managementUrl;
  std::string httpUrl;
  std::string email;
  std::string email2;
  std::string appId;
};

/// Load the config or skip the test (exit 77).
inline E2eConfig requireConfig(bool needSecondPlayer = false) {
  E2eConfig cfg;
  cfg.managementUrl = envOr("CROWDY_E2E_MANAGEMENT_URL");
  cfg.httpUrl = envOr("CROWDY_E2E_HTTP_URL");
  cfg.email = envOr("CROWDY_E2E_EMAIL");
  cfg.email2 = envOr("CROWDY_E2E_EMAIL_2");
  cfg.appId = envOr("CROWDY_E2E_APP_ID");
  if (cfg.managementUrl.empty() || cfg.email.empty() || cfg.appId.empty() ||
      (needSecondPlayer && cfg.email2.empty())) {
    std::puts("CROWDY_E2E_* not configured; skipping");
    std::exit(77);
  }
  return cfg;
}

/// One signed-in player with an app-scoped game client and a connected
/// native-UDP replication session.
struct Player {
  std::unique_ptr<crowdy::CrowdyClient> identity;
  std::unique_ptr<crowdy::CrowdyClient> game;
  crowdy::domains::AppTokenResponse appToken;
  std::string userId;
  std::shared_ptr<crowdy::replication::Connection> conn;

  crowdy::replication::TokenInfo tokenInfo() const {
    crowdy::replication::TokenInfo t;
    t.token = appToken.token;
    t.gameTokenId = appToken.gameTokenId;
    t.expiresAtEpochMs =
        crowdy::core::parseIso8601Millis(appToken.expiresAt.data(), appToken.expiresAt.size());
    return t;
  }
};

/// Sign in (dev bypass), mint the app token, build the per-game client.
inline Player signIn(const E2eConfig& cfg, const std::string& email) {
  Player p;
  crowdy::ClientConfig identityCfg;
  identityCfg.managementUrl = cfg.managementUrl;
  p.identity = std::make_unique<crowdy::CrowdyClient>(std::move(identityCfg));
  auto auth = p.identity->auth().devLogin(email);
  E2E_CHECK(!auth.token.empty());
  p.userId = auth.userId;

  p.appToken = p.identity->portal().mintAppToken(cfg.appId);
  E2E_CHECK(p.appToken.token.size() == 64);

  const std::string gameUrl =
      !cfg.httpUrl.empty() ? cfg.httpUrl
                           : (!p.appToken.gameApiUrl.empty() ? p.appToken.gameApiUrl
                                                             : cfg.managementUrl);
  crowdy::ClientConfig gameCfg;
  gameCfg.httpUrl = gameUrl;
  gameCfg.managementUrl = cfg.managementUrl;
  p.game = std::make_unique<crowdy::CrowdyClient>(std::move(gameCfg));
  p.game->setToken(p.appToken.token);
  return p;
}

/// Assign a replication server and open the native UDP connection
/// (owned-thread mode), waiting for session-ready.
inline void connectUdp(Player& p, const E2eConfig& cfg) {
  crowdy::replication::Config rc;
  rc.appId = std::strtoll(cfg.appId.c_str(), nullptr, 10);
  rc.token = p.tokenInfo();
  p.conn = p.game->replication().connect(rc);
  E2E_CHECK(p.conn->connect().ok());
  // Session install is asynchronous server-side; wait for Connected.
  for (int i = 0; i < 100 && p.conn->state() != crowdy::replication::ConnState::Connected; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p.conn->poll();
  }
  E2E_CHECK(p.conn->state() == crowdy::replication::ConnState::Connected);
}

/// Poll until `done()` or the timeout elapses; returns done().
template <typename Fn>
inline bool pollUntil(crowdy::replication::Connection& conn, Fn&& done, int timeoutMs = 8000) {
  const auto start = std::chrono::steady_clock::now();
  while (!done()) {
    conn.poll();
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs))
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

}  // namespace e2e
