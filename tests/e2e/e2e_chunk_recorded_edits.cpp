// A block a hub places reaches a ChunkStore load (0.56.0). A hub writes voxels with the node
// API's `world.set_voxels`, which records the edit in the chunk's edit log and never in its stored
// `voxels`; since ck-api v2.33.0 the chunk reads return each recorded edit as a `voxelStates`
// entry, and the store puts them over the grid. Before 0.56.0 it read `voxels` alone, and the
// block was gone after a reload.
//
// An existing player places one block of dirt through a Blocks with Friends player hub (ck-exec
// node type `player`, keyed by the player's id: `inventory`, `can_build_at`, `place`, `mine`),
// then a fresh ChunkStore loads the chunk and must show it, while the chunk's stored grid is
// byte-identical to the one before. The block is mined back however the run ends. The hub reads
// the player's pose from the actors in the chunk, so the suite stands an actor beside the block
// over native UDP while it calls.
//
//   CROWDY_E2E_API_URL          the tier's shared origin
//   CROWDY_E2E_PLAYER_FILE      a JSON file {"email", "password"} of a player of that app who
//                               holds dirt and may build in the chunk; read here, never printed
//   CROWDY_E2E_HUB_EDIT_APP_ID  the app running that hub
//   CROWDY_E2E_HUB_EDIT_CHUNK   "x,y,z" of a stored chunk there the player may build in
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/domains/exec.hpp"
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

namespace {

struct CheckFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Throws rather than exits, so the block this suite placed is mined back on the way out.
#define LIVE_CHECK(cond)                                                                        \
  do {                                                                                          \
    if (!(cond)) throw CheckFailed(std::string(#cond) + " at line " + std::to_string(__LINE__)); \
  } while (0)

struct Voxel {
  int x = 0;
  int y = 0;
  int z = 0;
};

std::string text(const graphql::Json& v) { return v.isString() ? v.asString() : v.dump(); }

std::optional<wire::ChunkCoord> parseChunk(const std::string& value) {
  wire::ChunkCoord c{};
  char tail = 0;
  long long x = 0, y = 0, z = 0;
  if (std::sscanf(value.c_str(), "%lld,%lld,%lld%c", &x, &y, &z, &tail) != 3) return std::nullopt;
  c.x = x;
  c.y = y;
  c.z = z;
  return c;
}

/// The kit pose Blocks with Friends reads from an actor: x, y, z as f32 at 0, 4 and 8, the
/// client's clock (ms, f64) at 36.
std::vector<std::uint8_t> pose(double x, double y, double z) {
  std::vector<std::uint8_t> bytes(48, 0);
  const float at[3] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
  std::memcpy(bytes.data(), at, sizeof(at));
  const double clock = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count());
  std::memcpy(bytes.data() + 36, &clock, sizeof(clock));
  return bytes;
}

/// One player's hub over ck-exec, with an actor kept present beside `standAt` during each call.
struct Hub {
  CrowdyClient& game;
  replication::Connection& conn;
  std::shared_ptr<domains::ExecConnection> exec;
  std::string key;
  wire::ChunkCoord chunk{};
  core::ActorUuid uuid = core::generateActorUuid();
  double standAt[3] = {0, 0, 0};
  std::chrono::steady_clock::time_point lastPose{};

  void stand() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPose < std::chrono::milliseconds(400)) return;
    lastPose = now;
    const std::vector<std::uint8_t> state = pose(standAt[0], standAt[1], standAt[2]);
    (void)conn.sendActorUpdate({chunk, uuid, Bytes(state.data(), state.size()), 1, wire::DecayRate::None});
    conn.poll();
  }

  void wait(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      stand();
      game.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  /// The hub's answer. While ck-exec's manager fails over (a few seconds) a call is refused
  /// before it runs (`Unavailable`, `Moved`); it is asked again. `place` and `mine` carry an
  /// action id, so a call the hub did run answers the same receipt the second time.
  graphql::Json call(const std::string& method, const graphql::JVal& args) {
    for (int attempt = 1;; ++attempt) {
      std::optional<domains::ExecReply> reply;
      exec->call("player", key, method, args, [&](domains::ExecReply r) { reply = std::move(r); });
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!reply && std::chrono::steady_clock::now() < end) {
        stand();
        game.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!reply) throw CheckFailed(method + ": no reply in 30 s");
      if (reply->ok()) return reply->value();
      const std::string why = std::string(domains::execStatusName(reply->status)) + ": " + reply->message();
      if (reply->retryable() && !reply->rateLimited() && attempt < 8) {
        std::printf("  (%s refused before it ran, %s; again in 3 s)\n", method.c_str(), why.c_str());
        wait(3000);
        continue;
      }
      throw CheckFailed(method + " " + why);
    }
  }
};

graphql::JVal at(const wire::ChunkCoord& chunk, const Voxel& v, const std::string& actionId) {
  graphql::JVal p;
  p["actionId"] = actionId;
  p["chunkX"] = static_cast<std::int64_t>(chunk.x);
  p["chunkY"] = static_cast<std::int64_t>(chunk.y);
  p["chunkZ"] = static_cast<std::int64_t>(chunk.z);
  p["voxelX"] = static_cast<std::int64_t>(v.x);
  p["voxelY"] = static_cast<std::int64_t>(v.y);
  p["voxelZ"] = static_cast<std::int64_t>(v.z);
  return p;
}

/// The chunk as `getChunk` returns it: its stored grid (base64) and its recorded edits.
struct ChunkRead {
  std::string voxels;
  std::vector<std::pair<Voxel, int>> entries;
};

ChunkRead readChunk(CrowdyClient& game, const std::string& appId, const wire::ChunkCoord& c) {
  const graphql::Json chunk = game.chunks().get(appId, domains::ChunkRef{c.x, c.y, c.z});
  LIVE_CHECK(!chunk.isNull());  // CROWDY_E2E_HUB_EDIT_CHUNK must name a stored chunk
  ChunkRead read;
  read.voxels = chunk["voxels"].asString();
  chunk["voxelStates"].forEach([&](graphql::Json e) {
    read.entries.push_back({Voxel{static_cast<int>(e["voxelCoord"]["x"].asInt64()),
                                  static_cast<int>(e["voxelCoord"]["y"].asInt64()),
                                  static_cast<int>(e["voxelCoord"]["z"].asInt64())},
                            static_cast<int>(e["voxelType"].asInt64())});
  });
  return read;
}

std::optional<int> entryAt(const ChunkRead& read, const Voxel& v) {
  for (const auto& [voxel, type] : read.entries) {
    if (voxel.x == v.x && voxel.y == v.y && voxel.z == v.z) return type;
  }
  return std::nullopt;
}

/// What a fresh ChunkStore shows at `v` after loading the chunk's neighbourhood.
std::pair<int, std::vector<std::uint8_t>> load(e2e::Player& p, const std::string& appId,
                                               const wire::ChunkCoord& chunk, const Voxel& v) {
  ChunkStore store(*p.conn, &p.game->chunks(), appId, ChunkStore::Options{});
  LIVE_CHECK(store.ensureAround(chunk, 1) >= 1);
  const VoxelState* state = store.voxelStateAt(chunk, v.x, v.y, v.z);
  return {store.voxelTypeAt(chunk, v.x, v.y, v.z),
          state ? state->state : std::vector<std::uint8_t>{}};
}

}  // namespace

int main() {
  e2e::E2eConfig cfg;
  cfg.apiUrl = e2e::envOr("CROWDY_E2E_API_URL");
  const std::string playerFile = e2e::envOr("CROWDY_E2E_PLAYER_FILE");
  cfg.appId = e2e::envOr("CROWDY_E2E_HUB_EDIT_APP_ID");
  const std::optional<wire::ChunkCoord> chunk = parseChunk(e2e::envOr("CROWDY_E2E_HUB_EDIT_CHUNK"));
  if (cfg.apiUrl.empty() || playerFile.empty() || cfg.appId.empty() || !chunk) {
    std::puts("set CROWDY_E2E_API_URL, CROWDY_E2E_PLAYER_FILE, CROWDY_E2E_HUB_EDIT_APP_ID and "
              "CROWDY_E2E_HUB_EDIT_CHUNK (x,y,z); skipping");
    return 77;
  }
  if (!graphql::makeCurlWebSocketTransport()) {
    std::puts("no WebSocket transport in this build (libcurl older than 8.13); skipping");
    return 77;
  }

  E2E_SUBTEST("the player signs in and stands in the chunk over native UDP");
  std::ifstream in(playerFile, std::ios::binary);
  E2E_CHECK(in.good());
  std::ostringstream raw;
  raw << in.rdbuf();
  const graphql::Json who = graphql::Json::parse(raw.str());
  e2e::Player p;
  {
    ClientConfig c;
    c.httpUrl = cfg.apiUrl;
    p.identity = std::make_unique<CrowdyClient>(std::move(c));
    auto auth = p.identity->auth().login(who["email"].asString(), who["password"].asString());
    E2E_CHECK(!auth.token.empty());
    p.identity->setToken(auth.token);
    p.userId = auth.userId;
  }
  p.appToken = p.identity->portal().mintAppToken(cfg.appId);
  {
    ClientConfig c;
    c.httpUrl = !p.appToken.gameApiUrl.empty() ? p.appToken.gameApiUrl.valueOrEmpty() : cfg.apiUrl;
    if (!p.appToken.gameApiWsUrl.empty()) c.wsUrl = p.appToken.gameApiWsUrl.valueOrEmpty();
    if (!p.appToken.discoveryUrl.empty()) c.discoveryUrl = p.appToken.discoveryUrl.valueOrEmpty();
    p.game = std::make_unique<CrowdyClient>(std::move(c));
    p.game->setToken(p.appToken.token);
  }
  e2e::connectUdp(p, cfg, cfg.appId);
  std::printf("player %s, app %s, chunk %lld,%lld,%lld\n", p.userId.c_str(), cfg.appId.c_str(),
              static_cast<long long>(chunk->x), static_cast<long long>(chunk->y),
              static_cast<long long>(chunk->z));

  Hub hub{*p.game, *p.conn, p.game->exec().connect(cfg.appId, {.nodeType = "player", .key = p.userId}),
          p.userId, *chunk};
  const std::string run = e2e::runSuffix();
  std::optional<Voxel> placed;
  int failures = 0;
  try {
    E2E_SUBTEST("the hub holds dirt for the player, who may build here");
    const ChunkRead before = readChunk(*p.game, cfg.appId, *chunk);
    std::string stackId;
    hub.call("inventory", graphql::JVal())["stacks"].forEach([&](graphql::Json s) {
      if (stackId.empty() && s["item_id"].asString() == "dirt" && s["quantity"].asInt64() > 0)
        stackId = text(s["id"]);
    });
    LIVE_CHECK(!stackId.empty());  // the player holds no dirt
    graphql::JVal where;
    where["cx"] = static_cast<std::int64_t>(chunk->x);
    where["cy"] = static_cast<std::int64_t>(chunk->y);
    where["cz"] = static_cast<std::int64_t>(chunk->z);
    LIVE_CHECK(hub.call("can_build_at", where)["allowed"].asBool());

    E2E_SUBTEST("place one block through the hub (the node API writes it)");
    // Voxels whose last recorded edit is air first (an earlier run's, mined back), then a few
    // high in the middle of the chunk. The hub refuses one that is not air, water or lava.
    std::vector<Voxel> candidates;
    for (const auto& [voxel, type] : before.entries) {
      if (type == 0) candidates.push_back(voxel);
    }
    for (int y : {13, 14, 15}) {
      for (int x : {6, 9}) candidates.push_back(Voxel{x, y, 7});
    }
    const std::string stamp = R"({"actorUuid":")" + std::string(hub.uuid.begin(), hub.uuid.end()) + R"("})";
    const std::vector<std::uint8_t> state(stamp.begin(), stamp.end());
    int blockId = -1;
    for (std::size_t i = 0; i < candidates.size() && i < 12 && blockId < 0; ++i) {
      const Voxel v = candidates[i];
      hub.standAt[0] = static_cast<double>(chunk->x * 16 + v.x) + 0.5;
      hub.standAt[1] = static_cast<double>(chunk->y * 16 + v.y) + 1.0;
      hub.standAt[2] = static_cast<double>(chunk->z * 16 + v.z) + 2.0;
      hub.lastPose = {};
      hub.wait(1500);
      graphql::JVal args = at(*chunk, v, "e2e-cpp-place-" + run + "-" + std::to_string(i));
      args["stackId"] = stackId;
      args["stateBase64"] = core::base64Encode(Bytes(state.data(), state.size()));
      const graphql::Json reply = hub.call("place", args);
      if (reply["success"].asBool()) {
        placed = v;
        blockId = static_cast<int>(reply["blockId"].asInt64());
      } else {
        std::printf("  %d,%d,%d: %s\n", v.x, v.y, v.z, text(reply["reason"]).c_str());
      }
    }
    LIVE_CHECK(placed.has_value() && blockId > 0);
    std::printf("placed block %d at %d,%d,%d\n", blockId, placed->x, placed->y, placed->z);

    E2E_SUBTEST("the stored grid is unchanged; the edit is a voxelStates entry");
    const ChunkRead after = readChunk(*p.game, cfg.appId, *chunk);
    LIVE_CHECK(after.voxels == before.voxels);
    LIVE_CHECK(entryAt(after, *placed) == blockId);

    E2E_SUBTEST("a fresh ChunkStore load shows the block and its state");
    const auto [type, loadedState] = load(p, cfg.appId, *chunk, *placed);
    std::printf("ChunkStore after the place: type %d, state %zu bytes\n", type, loadedState.size());
    LIVE_CHECK(type == blockId);
    LIVE_CHECK(loadedState == state);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    ++failures;
  }

  if (placed) {
    E2E_SUBTEST("mine it back; the next load shows air there");
    try {
      const graphql::Json mined = hub.call("mine", at(*chunk, *placed, "e2e-cpp-mine-" + run));
      LIVE_CHECK(mined["success"].asBool());
      LIVE_CHECK(load(p, cfg.appId, *chunk, *placed).first == 0);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FAILED to mine back %d,%d,%d: %s\n", placed->x, placed->y, placed->z,
                   e.what());
      ++failures;
    }
  }
  hub.exec->close();
  p.conn->disconnect();
  if (failures > 0) return 1;
  std::puts("e2e_chunk_recorded_edits OK");
  return 0;
}
