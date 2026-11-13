// ChunkStore on the game thread (0.82.0), against the Unreal team's measurements:
//   - 125 chunks with 256 recorded edits each (64-byte states), read through the Game API's
//     source: the parse runs on the store's I/O thread; tick() applies within its budget;
//   - range 8, 4,913 chunks, most of them air: memory, and the longest tick while they arrive.
// Offline: a fake transport answers every region read with the same pages.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/domains/world_data.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/chunk_store.hpp"

using namespace crowdy;
using namespace crowdy::session;
using Clock = std::chrono::steady_clock;

namespace {

struct NoProvider final : replication::ISessionProvider {
  Result<replication::Assignment> assignServer() override { return Errc::NotConnected; }
  Result<replication::TokenInfo> refreshToken() override { return Errc::NotConnected; }
};

/// Answers getChunksByDistance with the page of `chunks` the request's skip and limit name, as the
/// server does (whatever the box: the bench asks for one).
class Pages final : public graphql::IHttpTransport {
 public:
  std::vector<std::string> chunks;
  std::size_t bytes = 0;
  graphql::HttpResponse send(const graphql::HttpRequest& r) override { return sendOutcome(r).response; }
  graphql::HttpOutcome sendOutcome(const graphql::HttpRequest& request) noexcept override {
    const graphql::Json body = graphql::Json::parse(request.body);
    const auto skip = static_cast<std::size_t>(body["variables"]["input"]["skip"].asInt64(0));
    const auto limit = static_cast<std::size_t>(body["variables"]["input"]["limit"].asInt64(1000));
    std::string out = R"({"data":{"getChunksByDistance":{"limit":)" + std::to_string(limit) +
                      R"(,"skip":)" + std::to_string(skip) + R"(,"chunks":[)";
    for (std::size_t i = skip; i < std::min(chunks.size(), skip + limit); ++i) {
      if (i > skip) out += ",";
      out += chunks[i];
    }
    out += "]}}}";
    bytes += out.size();
    return {Errc::Ok, {200, out}, {}};
  }
};

std::string chunkJson(std::int64_t x, std::int64_t y, std::int64_t z, int edits, int stateBytes,
                      bool air) {
  std::vector<std::uint8_t> grid(kChunkVolume, 0);
  if (!air)
    for (int i = 0; i < kChunkVolume; ++i) grid[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i % 7);
  std::string out = R"({"coordinates":{"x":")" + std::to_string(x) + R"(","y":")" + std::to_string(y) +
                    R"(","z":")" + std::to_string(z) + R"("},"voxels":")" +
                    core::base64Encode(Bytes(grid.data(), grid.size())) + R"(","voxelStates":[)";
  const std::vector<std::uint8_t> state(static_cast<std::size_t>(stateBytes), 0x5a);
  const std::string stateB64 = core::base64Encode(Bytes(state.data(), state.size()));
  for (int i = 0; i < edits; ++i) {
    if (i) out += ",";
    out += R"({"voxelCoord":{"x":)" + std::to_string(i % 16) + R"(,"y":)" + std::to_string((i / 16) % 16) +
           R"(,"z":)" + std::to_string(i / 256) + R"(},"voxelType":3,"state":")" + stateB64 +
           // Recorded after the row was written, as ck-api v2.69.0 dates an edit.
           R"(","updatedAt":"2026-11-11T20:00:01.000Z"})";
  }
  out += R"(],"voxelStatesTruncated":false,"updatedAt":"2026-11-11T20:00:00.000Z"})";
  return out;
}

struct Run {
  double totalTickMs = 0;
  double longestTickMs = 0;
  int ticks = 0;
  double wallMs = 0;
};

Run loadAsync(ChunkStore& store, const ChunkCoord& center, int distance) {
  Run run;
  const auto start = Clock::now();
  store.ensureAroundAsync(center, distance);
  while (store.loadsInFlight() > 0) {
    const auto t0 = Clock::now();
    store.tick(run.ticks);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    run.totalTickMs += ms;
    run.longestTickMs = std::max(run.longestTickMs, ms);
    ++run.ticks;
  }
  run.wallMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  return run;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  replication::Connection conn{replication::Config{}, std::make_shared<NoProvider>(),
                               core::defaultCrypto()};

  // 1. 125 chunks (a 5^3 box), 256 edits each, 64-byte states: 2.7 MB of JSON per read.
  {
    auto http = std::make_shared<Pages>();
    for (int x = -2; x <= 2; ++x)
      for (int y = -2; y <= 2; ++y)
        for (int z = -2; z <= 2; ++z) http->chunks.push_back(chunkJson(x, y, z, 256, 64, false));
    auto gql = std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://bench/graphql", 60000},
                                                        http, std::make_shared<graphql::AuthState>());
    domains::ChunksAPI api{gql};
    std::size_t json = 0;
    for (const std::string& c : http->chunks) json += c.size();
    std::printf("125 chunks x 256 edits (64-byte states), %.1f MB of JSON\n", static_cast<double>(json) / 1e6);

    // The parse the I/O thread does (before 0.82.0 this ran on the caller's thread).
    auto source = makeGameApiChunkSource(api);
    const auto p0 = Clock::now();
    const auto parsed = source->chunksInBox("42", {-2, -2, -2}, {2, 2, 2}, {});
    std::printf("  read + parse on the I/O thread: %.1f ms (%zu chunks)\n",
                std::chrono::duration<double, std::milli>(Clock::now() - p0).count(), parsed.size());

    for (std::int64_t budgetUs : {2000, 500}) {
      ChunkStore::Options options;
      options.adoptBudgetUs = budgetUs;
      ChunkStore store(conn, &api, "42", options);
      const Run run = loadAsync(store, {0, 0, 0}, 2);
      std::printf("  tick budget %lld us: longest tick %.2f ms, %d ticks, %.1f ms on the game thread in all, "
                  "%.1f ms wall\n",
                  static_cast<long long>(budgetUs), run.longestTickMs, run.ticks, run.totalTickMs, run.wallMs);
      if (budgetUs == 2000) {
        std::size_t held = 0;
        for (const ChunkData* c : store.list()) held += c->memoryBytes();
        std::printf("  held: %.2f MB (the edits' times included)\n", static_cast<double>(held) / 1e6);
      }
    }
    ChunkStore blocking(conn, &api, "42", ChunkStore::Options{});
    const auto b0 = Clock::now();
    blocking.ensureAround({0, 0, 0}, 2);
    std::printf("  ensureAround (blocking, for comparison): %.1f ms\n",
                std::chrono::duration<double, std::milli>(Clock::now() - b0).count());
  }

  // 2. Range 8: 4,913 chunks, 1 in 8 terrain, the rest air with no edits.
  {
    auto http = std::make_shared<Pages>();
    for (int x = -8; x <= 8; ++x)
      for (int y = -8; y <= 8; ++y)
        for (int z = -8; z <= 8; ++z) {
          const bool terrain = ((x + y + z) & 7) == 0;
          http->chunks.push_back(chunkJson(x, y, z, terrain ? 16 : 0, 16, !terrain));
        }
    auto gql = std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://bench/graphql", 60000},
                                                        http, std::make_shared<graphql::AuthState>());
    domains::ChunksAPI api{gql};
    ChunkStore store(conn, &api, "42", ChunkStore::Options{});
    const Run run = loadAsync(store, {0, 0, 0}, 8);
    std::size_t bytes = 0, uniform = 0;
    for (const ChunkData* c : store.list()) {
      bytes += c->memoryBytes();
      uniform += c->uniform() ? 1 : 0;
    }
    std::printf("range 8: %zu chunks (%zu uniform), %.1f MB held (a 4 KiB grid each would be %.1f MB)\n",
                store.size(), uniform, static_cast<double>(bytes) / 1e6,
                static_cast<double>(store.size()) * kChunkVolume / 1e6);
    std::printf("  longest tick %.2f ms, %d ticks, %.1f ms on the game thread, %.1f ms wall\n",
                run.longestTickMs, run.ticks, run.totalTickMs, run.wallMs);
  }
  return 0;
}
