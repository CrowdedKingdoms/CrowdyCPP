// Open grids and the ck-exec gateway, against a tier, as a THROWAWAY org admin: the suite registers
// its own owner (with an org and an app) and a player, so it needs no account of anyone's.
// CrowdyJS's test/e2e/open-grid-and-exec-gateway.test.mjs makes the same checks.
//
//   CROWDY_E2E_API_URL=https://ck.dev.crowdedkingdoms.com CROWDY_E2E_EMAIL=you@example.com \
//   CROWDY_E2E_THROWAWAY_OWNER=1 ./e2e_open_grid_exec_gateway
//
// It registers two plus-addressed accounts per run and leaves an org behind (the app is
// archived), so it runs only when asked to. The gateway half needs a WebSocket transport
// (libcurl 8.13 or newer) and a tier with ck-exec; without either it says so and skips that half.
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

std::string str(const graphql::Json& v) { return v.isString() ? v.asString() : v.dump(); }

std::unique_ptr<CrowdyClient> gameClient(CrowdyClient& identity, const e2e::E2eConfig& cfg,
                                         const std::string& appId) {
  domains::AppTokenResponse minted = identity.portal().mintAppToken(appId);
  ClientConfig c;
  c.httpUrl = !minted.gameApiUrl.empty() ? minted.gameApiUrl.valueOrEmpty() : cfg.apiUrl;
  if (!minted.gameApiWsUrl.empty()) c.wsUrl = minted.gameApiWsUrl.valueOrEmpty();
  if (!minted.discoveryUrl.empty()) c.discoveryUrl = minted.discoveryUrl.valueOrEmpty();
  auto client = std::make_unique<CrowdyClient>(std::move(c));
  client->setToken(minted.token);
  return client;
}

graphql::JVal chunk(long x, long y, long z) {
  graphql::JVal c;
  c["x"] = std::to_string(x);
  c["y"] = std::to_string(y);
  c["z"] = std::to_string(z);
  return c;
}

graphql::JVal openInput(const std::string& appId, const std::string& gridId, std::vector<std::string> keys) {
  graphql::JVal input;
  input["appId"] = appId;
  input["gridId"] = gridId;
  graphql::JArray list;
  for (auto& k : keys) list.emplace_back(std::move(k));
  input["permissionKeys"] = graphql::JVal(std::move(list));
  return input;
}

std::vector<std::string> keysOf(const graphql::Json& v) {
  std::vector<std::string> out;
  v["permissionKeys"].forEach([&](graphql::Json k) { out.push_back(k.asString()); });
  return out;
}

bool holds(const graphql::Json& v, const std::string& key) {
  for (const auto& k : keysOf(v)) {
    if (k == key) return true;
  }
  return false;
}

template <typename Read, typename Ok>
graphql::Json until(Read&& read, Ok&& ok, int ms = 30000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  graphql::Json last = read();
  while (!ok(last) && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    last = read();
  }
  return last;
}

template <typename Drain, typename Done>
bool drainUntil(Drain&& drain, Done&& done, int ms = 15000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    drain();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  drain();
  return done();
}

void openGrids(const std::string& appId, CrowdyClient& owner, const std::string& playerId) {
  auto& grids = owner.gameApps();
  E2E_SUBTEST("an org admin opens a grid nested in the world grid");
  std::mt19937 rng(std::random_device{}());
  const long x = 200 + static_cast<long>(rng() % 1000);
  graphql::JVal box;
  box["appId"] = appId;
  box["corner1"] = chunk(x, 0, x);
  box["corner2"] = chunk(x + 3, 0, x + 3);
  graphql::Json created = until([&] { return grids.createGrid(box); },
                                [](const graphql::Json& r) { return r["error"].asString() != "NO_MATCHING_GRID_ASSIGNMENT"; });
  E2E_CHECK(created["error"].asString() == "NO_ERROR");
  const std::string gridId = str(created["grid"]["grid_id"]);
  std::printf("app %s, grid %s, player %s\n", appId.c_str(), gridId.c_str(), playerId.c_str());

  E2E_CHECK(keysOf(grids.openPermissions(appId, gridId)).empty());
  graphql::Json opened = grids.setOpenPermissions(openInput(appId, gridId, {"update_voxel_data", "access"}));
  E2E_CHECK(str(opened["gridId"]) == gridId);
  E2E_CHECK((keysOf(opened) == std::vector<std::string>{"access", "update_voxel_data"}));
  E2E_CHECK((keysOf(grids.openPermissions(appId, gridId)) == std::vector<std::string>{"access", "update_voxel_data"}));

  E2E_SUBTEST("every player with access holds the open keys");
  graphql::Json held = until([&] { return grids.userPermissions(appId, gridId, playerId); },
                             [](const graphql::Json& p) { return holds(p, "access") && holds(p, "update_voxel_data"); });
  E2E_CHECK(holds(held, "update_voxel_data"));

  E2E_SUBTEST("a player-code key is refused BAD_REQUEST and changes nothing");
  bool refused = false;
  try {
    (void)grids.setOpenPermissions(openInput(appId, gridId, {"write_server_code"}));
  } catch (const graphql::CrowdyGraphQLError& e) {
    refused = e.code() == "BAD_REQUEST";
    std::printf("refused: %s %s\n", e.code().c_str(), e.what());
  }
  E2E_CHECK(refused);
  E2E_CHECK((keysOf(grids.openPermissions(appId, gridId)) == std::vector<std::string>{"access", "update_voxel_data"}));

  E2E_SUBTEST("closing it takes the keys back");
  E2E_CHECK(keysOf(grids.setOpenPermissions(openInput(appId, gridId, {}))).empty());
  graphql::Json after = until([&] { return grids.userPermissions(appId, gridId, playerId); },
                              [](const graphql::Json& p) { return !holds(p, "update_voxel_data"); });
  E2E_CHECK(!holds(after, "update_voxel_data"));
}

void gateway(const std::string& appId, CrowdyClient& player) {
  E2E_SUBTEST("the tier's gateway passes the pin, and a tampered connect token is Denied");
  auto transport = graphql::makeCurlWebSocketTransport();
  if (!transport) {
    std::puts("no WebSocket transport in this build (libcurl older than 8.13); the gateway half is skipped");
    return;
  }
  Result<domains::ExecEndpoint> endpoint = player.exec().endpoint(appId);
  if (!endpoint.ok()) {
    std::printf("execConnect failed (%s): no ck-exec here; the gateway half is skipped\n", errcName(endpoint.error()));
    return;
  }
  const std::string api = player.graphqlClient().endpoint();
  std::printf("game API %s, gateway %s\n", api.c_str(), endpoint->gatewayUrl.c_str());
  E2E_CHECK(!domains::execGatewayRefusal(api, endpoint->gatewayUrl).has_value());
  E2E_CHECK(endpoint->gatewayUrl.rfind("wss://", 0) == 0);

  auto conn = player.exec().connect(appId);
  std::optional<domains::ExecReply> pong;
  conn->ping([&](domains::ExecReply r) { pong = std::move(r); });
  E2E_CHECK(drainUntil([&] { player.poll(); }, [&] { return pong.has_value(); }));
  E2E_CHECK(pong->ok());
  conn->close();

  domains::ExecEndpoint tampered = endpoint.value();
  tampered.token = tampered.token.substr(0, tampered.token.size() - 4) + "AAAA";
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  auto refused = domains::ExecConnection::open(transport, dispatcher, tampered);
  std::optional<domains::ExecReply> denied;
  refused->ping([&](domains::ExecReply r) { denied = std::move(r); });
  E2E_CHECK(drainUntil([&] { dispatcher->drain(); }, [&] { return denied.has_value(); }));
  std::printf("tampered token: %s: %s\n", std::string(domains::execStatusName(denied->status)).c_str(),
              denied->message().c_str());
  E2E_CHECK(denied->status == domains::ExecStatus::Denied);
  E2E_CHECK(denied->message().rfind("the gateway refused the connection (HTTP 401", 0) == 0);
  E2E_CHECK(refused->lastFailure().has_value() && refused->lastFailure()->status == domains::ExecStatus::Denied);
  refused->close();
}

}  // namespace

int main() {
  e2e::E2eConfig cfg;
  cfg.apiUrl = e2e::envOr("CROWDY_E2E_API_URL");
  cfg.email = e2e::envOr("CROWDY_E2E_EMAIL");
  if (cfg.apiUrl.empty() || cfg.email.empty() || !e2e::envFlag("CROWDY_E2E_THROWAWAY_OWNER")) {
    std::puts("set CROWDY_E2E_API_URL, CROWDY_E2E_EMAIL and CROWDY_E2E_THROWAWAY_OWNER=1 (registers throwaway accounts); skipping");
    return 77;
  }

  E2E_SUBTEST("a throwaway owner, org and app, and a player");
  auto owner = e2e::identityClient(cfg, e2e::deriveEmail(cfg, "open-grid-owner"));
  graphql::JVal orgInput;
  orgInput["name"] = "CrowdyCPP e2e open grids " + e2e::runSuffix();
  orgInput["slug"] = "cpp-e2e-open-grids-" + e2e::runSuffix();
  const std::string orgId = str(owner->admin().organizations().create(orgInput)["orgId"]);
  graphql::JVal appInput;
  appInput["orgId"] = orgId;
  appInput["name"] = "CrowdyCPP e2e open grids " + e2e::runSuffix();
  appInput["slug"] = "cpp-e2e-open-grids-" + e2e::runSuffix();
  appInput["datacenter"] = e2e::placeableDatacenter(*owner);
  const std::string appId = str(owner->admin().apps().create(appInput)["appId"]);
  E2E_CHECK(!appId.empty() && appId != "0");
  auto ownerGame = gameClient(*owner, cfg, appId);
  std::string playerId;
  auto playerIdentity = e2e::identityClient(cfg, e2e::deriveEmail(cfg, "open-grid-player"), &playerId);
  auto player = gameClient(*playerIdentity, cfg, appId);

  openGrids(appId, *ownerGame, playerId);
  gateway(appId, *player);

  E2E_CHECK(owner->admin().apps().archive(appId)["status"].asString() == "ARCHIVED");
  std::puts("e2e_open_grid_exec_gateway OK");
  return 0;
}
