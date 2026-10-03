// ChunkStore durable write-back failures: a refusal the server will not change
// is sent once and dropped, a failure that can clear is retried with backoff and
// then dropped, and neither holds up the other chunks' write-backs.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/domains/world_data.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/chunk_store.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

namespace {

/// How the fake Game API answers UpdateChunk for a chunk whose x coordinate is
/// the key; other chunks are stored.
enum class Answer {
  Forbidden,            // GraphQL FORBIDDEN, extensions.httpStatus 403
  BadRequest,           // HTTP 400 carrying a BAD_REQUEST error
  BusyTwice,            // PLATFORM_BUSY twice, then stored
  NetworkDown,          // the request never completes
  UnauthenticatedOnce,  // UNAUTHENTICATED once, then stored
  NotRetryable,         // an unknown code with extensions.retryable: false
  PayloadTooLarge,      // HTTP 413 with a non-GraphQL body
  ServerErrorOnce,      // HTTP 503 once, then stored
};

class ChunkApiTransport final : public graphql::IHttpTransport {
 public:
  std::map<std::int64_t, Answer> answers;
  std::map<std::int64_t, int> calls;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    return sendOutcome(request).response;
  }

  graphql::HttpOutcome sendOutcome(const graphql::HttpRequest& request) noexcept override {
    const graphql::Json body = graphql::Json::parse(request.body);
    const std::int64_t x = body["variables"]["input"]["coordinates"]["x"].asBigInt(-1);
    const int n = ++calls[x];
    const auto it = answers.find(x);
    if (it == answers.end()) return stored();
    switch (it->second) {
      case Answer::Forbidden:
        return {Errc::Ok,
                {200, R"({"errors":[{"message":"You do not have permission to write this chunk","extensions":{"code":"FORBIDDEN","httpStatus":403}}],"data":null})"},
                {}};
      case Answer::BadRequest:
        return {Errc::Ok,
                {400, R"({"errors":[{"message":"voxels must be 4096 bytes","extensions":{"code":"BAD_REQUEST","httpStatus":400}}]})"},
                {}};
      case Answer::BusyTwice:
        if (n > 2) return stored();
        return {Errc::Ok,
                {200, R"({"errors":[{"message":"busy","extensions":{"code":"PLATFORM_BUSY","retryable":true,"httpStatus":503}}],"data":null})"},
                {}};
      case Answer::NetworkDown:
        return {Errc::SocketError, {}, "connection refused"};
      case Answer::UnauthenticatedOnce:
        if (n > 1) return stored();
        return {Errc::Ok,
                {200, R"({"errors":[{"message":"token expired","extensions":{"code":"UNAUTHENTICATED","httpStatus":401}}],"data":null})"},
                {}};
      case Answer::NotRetryable:
        return {Errc::Ok,
                {200, R"({"errors":[{"message":"no","extensions":{"code":"SOMETHING_NEW","retryable":false}}],"data":null})"},
                {}};
      case Answer::PayloadTooLarge:
        return {Errc::Ok, {413, "request entity too large"}, {}};
      case Answer::ServerErrorOnce:
        if (n > 1) return stored();
        return {Errc::Ok, {503, "service unavailable"}, {}};
    }
    return stored();
  }

 private:
  static graphql::HttpOutcome stored() {
    return {Errc::Ok, {200, R"({"data":{"updateChunk":{"appId":"42"}}})"}, {}};
  }
};

struct NoProvider final : replication::ISessionProvider {
  Result<replication::Assignment> assignServer() override { return Errc::NotConnected; }
  Result<replication::TokenInfo> refreshToken() override { return Errc::NotConnected; }
};

/// A ChunkStore over the fake Game API; the replication connection is never opened.
struct Fixture {
  std::shared_ptr<ChunkApiTransport> http = std::make_shared<ChunkApiTransport>();
  std::shared_ptr<graphql::GraphQLClient> gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
      std::make_shared<graphql::AuthState>());
  domains::ChunksAPI chunksApi{gql};
  replication::Connection conn{replication::Config{}, std::make_shared<NoProvider>(),
                               core::defaultCrypto()};
  std::vector<std::int64_t> sleeps;
  std::vector<ChunkWriteBackFailure> reported;
  std::unique_ptr<ChunkStore> store;

  Fixture() {
    ChunkStore::Options options;
    options.writeBackIntervalMs = 100;
    options.sleep = [this](std::int64_t ms) { sleeps.push_back(ms); };
    store = std::make_unique<ChunkStore>(conn, &chunksApi, "42", options);
    store->onWriteBackFailed(
        [this](const ChunkWriteBackFailure& failure) { reported.push_back(failure); });
  }

  void seed(std::int64_t x, Answer answer) {
    http->answers[x] = answer;
    seed(x);
  }
  void seed(std::int64_t x) { store->seed(at(x), {}); }

  static ChunkCoord at(std::int64_t x) { return ChunkCoord{x, 0, 0}; }

  const ChunkWriteBackFailure* reportedFor(std::int64_t x) const {
    for (const auto& failure : reported)
      if (failure.coord == at(x)) return &failure;
    return nullptr;
  }
};

bool hasCode(const ChunkWriteBackFailure& failure, const char* code) {
  return !failure.error.errors.empty() && failure.error.errors.front().code == code;
}

// The throttled tick loop: refused chunks are sent once, busy/network ones are
// retried on the 0.7/1.4/2.8/5.6 s schedule, and the rest persist regardless.
void testTickDropsRefusalsAndRetriesTheRest() {
  Fixture f;
  f.seed(1, Answer::Forbidden);
  f.seed(2, Answer::BadRequest);
  f.seed(3, Answer::BusyTwice);
  f.seed(4, Answer::NetworkDown);
  f.seed(5, Answer::UnauthenticatedOnce);
  f.seed(6, Answer::NotRetryable);
  f.seed(7, Answer::PayloadTooLarge);
  f.seed(8, Answer::ServerErrorOnce);
  f.seed(9);
  CHECK_EQ(f.store->pendingWriteBacks(), 9u);

  // Enough 100 ms ticks for every backoff (0.7 + 1.4 + 2.8 + 5.6 s) to run out.
  for (std::int64_t now = 1000; now <= 30000; now += 100) f.store->tick(now);

  CHECK_EQ(f.store->pendingWriteBacks(), 0u);
  CHECK_EQ(f.http->calls[1], 1);
  CHECK_EQ(f.http->calls[2], 1);
  CHECK_EQ(f.http->calls[3], 3);
  CHECK_EQ(f.http->calls[4], 5);
  CHECK_EQ(f.http->calls[5], 2);
  CHECK_EQ(f.http->calls[6], 1);
  CHECK_EQ(f.http->calls[7], 1);
  CHECK_EQ(f.http->calls[8], 2);
  CHECK_EQ(f.http->calls[9], 1);

  for (std::int64_t x : {3, 5, 8, 9}) {
    const ChunkData* c = f.store->find(Fixture::at(x));
    CHECK(c != nullptr && !c->dirty && c->storedOnServer);
    CHECK(f.reportedFor(x) == nullptr);
  }

  CHECK_EQ(f.reported.size(), 5u);
  for (std::int64_t x : {1, 2, 6, 7}) {
    const ChunkWriteBackFailure* failure = f.reportedFor(x);
    CHECK(failure != nullptr);
    CHECK(failure->reason == ChunkWriteBackDrop::Refused);
    CHECK_EQ(failure->attempts, 1);
    const ChunkData* c = f.store->find(Fixture::at(x));
    CHECK(c != nullptr && !c->dirty && !c->storedOnServer);  // local voxels kept
  }
  CHECK(hasCode(*f.reportedFor(1), "FORBIDDEN"));
  CHECK(f.reportedFor(1)->error.errors.front().httpStatus == 403);
  CHECK(hasCode(*f.reportedFor(2), "BAD_REQUEST"));
  CHECK_EQ(f.reportedFor(2)->error.httpStatus, 400);
  CHECK(hasCode(*f.reportedFor(6), "SOMETHING_NEW"));
  CHECK_EQ(f.reportedFor(7)->error.httpStatus, 413);

  const ChunkWriteBackFailure* exhausted = f.reportedFor(4);
  CHECK(exhausted != nullptr);
  CHECK(exhausted->reason == ChunkWriteBackDrop::Exhausted);
  CHECK_EQ(exhausted->attempts, 5);
  CHECK(exhausted->error.kind == graphql::GraphQLErrorKind::Network);
}

// One refused chunk must not hold up the others: each persists on its own tick.
void testARefusedChunkDoesNotBlockTheOthers() {
  Fixture f;
  f.seed(1, Answer::Forbidden);
  f.seed(2);
  f.seed(3);
  f.seed(4);
  for (std::int64_t now = 1000; now < 1000 + 4 * 100; now += 100) f.store->tick(now);
  CHECK_EQ(f.store->pendingWriteBacks(), 0u);
  CHECK_EQ(f.http->calls[1], 1);
  for (std::int64_t x : {2, 3, 4}) CHECK(f.store->find(Fixture::at(x))->storedOnServer);
}

// A chunk waiting out its backoff does not hold up the others either.
void testABackingOffChunkDoesNotBlockTheOthers() {
  Fixture f;
  f.seed(1, Answer::NetworkDown);
  f.store->tick(1000);  // attempt 1 fails; next try due at 1700
  f.seed(2);
  f.store->tick(1100);
  CHECK(f.store->find(Fixture::at(2))->storedOnServer);
  CHECK_EQ(f.http->calls[1], 1);
  f.store->tick(1600);  // chunk 1 is not due yet
  CHECK_EQ(f.http->calls[1], 1);
  f.store->tick(1700);
  CHECK_EQ(f.http->calls[1], 2);
}

// flush() waits out the backoff, reports what it dropped, and a re-seeded
// refused chunk is sent once more (and dropped again).
void testFlushReportsDroppedWriteBacks() {
  Fixture f;
  f.seed(1, Answer::Forbidden);
  f.seed(2, Answer::BadRequest);
  f.seed(3, Answer::BusyTwice);
  f.seed(4, Answer::NetworkDown);
  f.seed(5);

  const ChunkFlushResult result = f.store->flush();
  CHECK_EQ(result.persisted, 2u);
  CHECK_EQ(result.dropped.size(), 3u);
  CHECK_EQ(f.reported.size(), 3u);
  CHECK_EQ(f.store->pendingWriteBacks(), 0u);
  CHECK_EQ(f.http->calls[1], 1);
  CHECK_EQ(f.http->calls[2], 1);
  CHECK_EQ(f.http->calls[3], 3);
  CHECK_EQ(f.http->calls[4], 5);
  for (const auto& dropped : result.dropped) {
    if (dropped.coord == Fixture::at(4)) {
      CHECK(dropped.reason == ChunkWriteBackDrop::Exhausted);
      CHECK_EQ(dropped.attempts, 5);
    } else {
      CHECK(dropped.coord == Fixture::at(1) || dropped.coord == Fixture::at(2));
      CHECK(dropped.reason == ChunkWriteBackDrop::Refused);
      CHECK_EQ(dropped.attempts, 1);
    }
  }
  std::vector<std::int64_t> waits = f.sleeps;
  std::sort(waits.begin(), waits.end());
  const std::vector<std::int64_t> expected{700, 700, 1400, 1400, 2800, 5600};
  CHECK(waits == expected);

  f.seed(1);
  const ChunkFlushResult again = f.store->flush();
  CHECK_EQ(again.persisted, 0u);
  CHECK_EQ(again.dropped.size(), 1u);
  CHECK(again.dropped.front().reason == ChunkWriteBackDrop::Refused);
  CHECK_EQ(f.http->calls[1], 2);
  CHECK_EQ(f.store->pendingWriteBacks(), 0u);
}

// pruneBeyond evicts a refused chunk instead of keeping it dirty forever; one
// whose failure can clear stays, dirty, for the next tick.
void testPruneBeyondEvictsRefusedChunks() {
  Fixture f;
  f.seed(0);
  f.seed(10, Answer::Forbidden);
  f.seed(11);
  f.seed(12, Answer::NetworkDown);

  const std::size_t pruned = f.store->pruneBeyond(Fixture::at(0), 2);
  CHECK_EQ(pruned, 2u);
  CHECK(f.store->find(Fixture::at(10)) == nullptr);
  CHECK(f.store->find(Fixture::at(11)) == nullptr);
  const ChunkData* retrying = f.store->find(Fixture::at(12));
  CHECK(retrying != nullptr && retrying->dirty);
  CHECK(f.store->find(Fixture::at(0)) != nullptr);
  CHECK_EQ(f.http->calls[10], 1);
  CHECK_EQ(f.reported.size(), 1u);
  CHECK(f.reported.front().coord == Fixture::at(10));
  CHECK(f.reported.front().reason == ChunkWriteBackDrop::Refused);
}

// A failure callback may change the store (here it caches enough chunks to rehash
// it) while pruneBeyond is evicting.
void testPruneBeyondSurvivesACallbackThatChangesTheStore() {
  Fixture f;
  f.seed(0);
  f.seed(10, Answer::Forbidden);
  f.store->onWriteBackFailed([&](const ChunkWriteBackFailure&) {
    for (std::int64_t y = 1; y <= 256; ++y) f.store->insertGenerated(ChunkCoord{0, y, 0}, {});
  });
  CHECK_EQ(f.store->pruneBeyond(Fixture::at(0), 2), 1u);
  CHECK(f.store->find(Fixture::at(10)) == nullptr);
  CHECK_EQ(f.store->size(), 257u);
}

}  // namespace

/// 0.54.0: an injected IChunkSource hydrates and writes back with the same
/// refusal / retry classification the Game API path uses.
class ScriptedSource final : public IChunkSource {
 public:
  std::vector<StoredChunk> stored;
  /// Per chunk x: outcomes handed out in order; once exhausted, the write succeeds.
  std::map<std::int64_t, std::vector<graphql::GraphQLOutcome>> answers;
  std::vector<ChunkCoord> written;
  int lastDistance = 0;

  std::vector<StoredChunk> chunksAround(const std::string& appId, const ChunkCoord&,
                                        int distance) override {
    CHECK_EQ(appId, std::string("42"));
    lastDistance = distance;
    return stored;
  }

  graphql::GraphQLOutcome writeChunk(const std::string& appId, const ChunkCoord& coord,
                                     Bytes voxels) override {
    CHECK_EQ(appId, std::string("42"));
    CHECK_EQ(voxels.size(), static_cast<std::size_t>(kChunkVolume));
    written.push_back(coord);
    auto& queue = answers[coord.x];
    if (queue.empty()) return graphql::GraphQLOutcome{};
    graphql::GraphQLOutcome out = queue.front();
    queue.erase(queue.begin());
    return out;
  }
};

graphql::GraphQLOutcome graphqlError(const char* code, bool retryable) {
  graphql::GraphQLOutcome out;
  out.status = Errc::Rejected;
  out.kind = graphql::GraphQLErrorKind::GraphQL;
  graphql::GraphQLErrorDetail detail;
  detail.code = code;
  detail.retryable = retryable;
  out.errors.push_back(detail);
  return out;
}

void testAnInjectedSourceHydratesAndWritesBack() {
  ScriptedSource source;
  StoredChunk stored;
  stored.coord = {5, 0, 0};
  stored.voxels.assign(kChunkVolume, 9);
  source.stored.push_back(stored);
  source.answers[1] = {graphqlError("FORBIDDEN", true)};
  source.answers[2] = {graphqlError("PLATFORM_BUSY", true)};

  replication::Connection conn{replication::Config{}, std::make_shared<NoProvider>(),
                               core::defaultCrypto()};
  ChunkStore::Options options;
  options.writeBackIntervalMs = 100;
  options.sleep = [](std::int64_t) {};
  ChunkStore store(conn, &source, "42", options);
  std::vector<ChunkWriteBackFailure> reported;
  store.onWriteBackFailed([&](const ChunkWriteBackFailure& f) { reported.push_back(f); });

  CHECK_EQ(store.ensureAround({0, 0, 0}, 12), 1u);
  CHECK_EQ(source.lastDistance, 8);  // clamped to the durable query's 1-8
  CHECK_EQ(store.voxelTypeAt({5, 0, 0}, 1, 2, 3), 9);

  store.seed({1, 0, 0}, {});
  store.seed({2, 0, 0}, {});
  ChunkFlushResult result = store.flush();
  CHECK_EQ(result.persisted, 1u);  // chunk 2, on its second attempt
  CHECK_EQ(result.dropped.size(), 1u);
  CHECK((result.dropped.front().coord == ChunkCoord{1, 0, 0}));
  CHECK(result.dropped.front().reason == ChunkWriteBackDrop::Refused);
  CHECK_EQ(result.dropped.front().attempts, 1);
  CHECK_EQ(reported.size(), 1u);
  CHECK_EQ(store.pendingWriteBacks(), 0u);

  ChunkStore offline(conn, nullptr, "42", options);  // no durable store
  CHECK_EQ(offline.ensureAround({0, 0, 0}, 1), 0u);
}

/// Answers every request with one getChunksByDistance response recorded from dev (ck-api
/// v2.33.0, 2026-10-03): three chunks of a voxel game's build area, the app id, chunk ids,
/// owner and the actor uuids inside voxel states replaced. Keeps the last request.
class RecordedChunksTransport final : public graphql::IHttpTransport {
 public:
  std::string response;
  std::string lastBody;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    return sendOutcome(request).response;
  }

  graphql::HttpOutcome sendOutcome(const graphql::HttpRequest& request) noexcept override {
    lastBody = request.body;
    return {Errc::Ok, {200, response}, {}};
  }
};

std::string readFixture(const char* name) {
  std::ifstream in(std::string(CROWDY_TEST_FIXTURE_DIR) + "/" + name, std::ios::binary);
  CHECK(in.good());
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::vector<std::uint8_t> decoded(const graphql::Json& base64) {
  auto bytes = core::base64Decode(base64.asStringView());
  return bytes ? *bytes : std::vector<std::uint8_t>{};
}

ChunkCoord coordOf(const graphql::Json& chunk) {
  return {chunk["coordinates"]["x"].asBigInt(), chunk["coordinates"]["y"].asBigInt(),
          chunk["coordinates"]["z"].asBigInt()};
}

// Since ck-api v2.33.0 every voxel edit recorded for a chunk (a hub's or mod's world.set_voxels,
// updateVoxel, a realtime voxel update) comes back as a voxelStates entry, and none of them is in
// `voxels`. The bulk load selects the entries and puts each over the stored grid.
void testABulkLoadAppliesTheRecordedEdits() {
  auto http = std::make_shared<RecordedChunksTransport>();
  http->response = readFixture("chunks-by-distance-recorded-edits.json");
  auto gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
      std::make_shared<graphql::AuthState>());
  domains::ChunksAPI chunksApi{gql};
  replication::Connection conn{replication::Config{}, std::make_shared<NoProvider>(),
                               core::defaultCrypto()};
  ChunkStore store(conn, &chunksApi, "42", ChunkStore::Options{});

  CHECK_EQ(store.ensureAround({2, 1, -1}, 1), 3u);
  const graphql::Json request = graphql::Json::parse(http->lastBody);
  CHECK(request["query"].asString().find("voxelStates") != std::string::npos);
  CHECK_EQ(request["variables"]["input"]["maxDistance"].asInt64(), 1);

  const graphql::Json fixture = graphql::Json::parse(http->response);
  graphql::Json placedChunk;
  std::size_t entries = 0;
  fixture["data"]["getChunksByDistance"]["chunks"].forEach([&](graphql::Json chunk) {
    const ChunkCoord coord = coordOf(chunk);
    if (coord == ChunkCoord{2, 1, -1}) placedChunk = chunk;
    // The stored grid (zeros for a chunk stored with `voxels: null`), each entry's type over it.
    std::vector<std::uint8_t> expected = decoded(chunk["voxels"]);
    if (expected.size() != static_cast<std::size_t>(kChunkVolume)) expected.assign(kChunkVolume, 0);
    chunk["voxelStates"].forEach([&](graphql::Json entry) {
      const int x = static_cast<int>(entry["voxelCoord"]["x"].asInt64());
      const int y = static_cast<int>(entry["voxelCoord"]["y"].asInt64());
      const int z = static_cast<int>(entry["voxelCoord"]["z"].asInt64());
      expected[static_cast<std::size_t>(voxelIndex(x, y, z))] =
          static_cast<std::uint8_t>(entry["voxelType"].asInt64());
      const VoxelState* state = store.voxelStateAt(coord, x, y, z);
      if (entry["state"].isString()) {
        CHECK(state != nullptr);
        CHECK_EQ(state->voxelType, entry["voxelType"].asInt64());
        CHECK(state->state == decoded(entry["state"]));
      } else {
        CHECK(state == nullptr);
      }
      ++entries;
    });
    const ChunkData* cached = store.find(coord);
    CHECK(cached != nullptr && cached->storedOnServer);
    CHECK(std::memcmp(cached->voxels.data(), expected.data(), expected.size()) == 0);
  });
  CHECK_EQ(entries, 16u);

  // Two edits disagree with the stored grid: a block placed where it held 5, and one mined where
  // it held 4. A load that read `voxels` alone showed neither.
  const ChunkCoord placed{2, 1, -1};
  const std::vector<std::uint8_t> storedGrid = decoded(placedChunk["voxels"]);
  CHECK_EQ(storedGrid[static_cast<std::size_t>(voxelIndex(11, 1, 4))], 5);
  CHECK_EQ(store.voxelTypeAt(placed, 11, 1, 4), 2);
  CHECK_EQ(storedGrid[static_cast<std::size_t>(voxelIndex(13, 2, 2))], 4);
  CHECK_EQ(store.voxelTypeAt(placed, 13, 2, 2), 0);
  const VoxelState* state = store.voxelStateAt(placed, 10, 1, 3);
  CHECK(state != nullptr);
  CHECK(std::string(state->state.begin(), state->state.end()) ==
        R"({"actorUuid":"00000000000000000000000000000001"})");
}

// A load takes the server's states over the cache: an entry's state replaces the cached one, an
// entry without one clears it, an entry outside the chunk is ignored, and a source that reports no
// states leaves the cached ones as they were.
void testALoadTakesTheServersStates() {
  ScriptedSource source;
  StoredChunk stored;
  stored.coord = {5, 0, 0};
  stored.voxels.assign(kChunkVolume, 9);
  stored.voxelStates.push_back({1, 2, 3, 7, {0xab}});
  stored.voxelStates.push_back({4, 5, 6, 0, {}});
  stored.voxelStates.push_back({16, 0, 0, 5, {0x01}});
  stored.voxelStates.push_back({0, -1, 0, 5, {0x01}});
  source.stored.push_back(stored);

  replication::Connection conn{replication::Config{}, std::make_shared<NoProvider>(),
                               core::defaultCrypto()};
  ChunkStore::Options options;
  options.writeBackIntervalMs = 0;  // the local edits below stay local
  ChunkStore store(conn, &source, "42", options);
  const ChunkCoord at{5, 0, 0};
  const std::uint8_t local[] = {0x55};
  (void)store.setVoxel(at, 1, 2, 3, 3, Bytes(local, 1), core::ActorUuid{});
  (void)store.setVoxel(at, 4, 5, 6, 3, Bytes(local, 1), core::ActorUuid{});
  CHECK(store.voxelStateAt(at, 4, 5, 6) != nullptr);

  CHECK_EQ(store.ensureAround(at, 1), 1u);
  CHECK_EQ(store.voxelTypeAt(at, 1, 2, 3), 7);
  const VoxelState* kept = store.voxelStateAt(at, 1, 2, 3);
  CHECK(kept != nullptr && kept->voxelType == 7 && kept->state == std::vector<std::uint8_t>{0xab});
  CHECK_EQ(store.voxelTypeAt(at, 4, 5, 6), 0);
  CHECK(store.voxelStateAt(at, 4, 5, 6) == nullptr);
  CHECK_EQ(store.find(at)->voxelStates.size(), 1u);
  CHECK_EQ(store.voxelTypeAt(at, 0, 1, 0), 9);  // where (16,0,0) would land unchecked
  for (int i = 0; i < kChunkVolume; ++i) {
    if (i == voxelIndex(1, 2, 3) || i == voxelIndex(4, 5, 6)) continue;
    CHECK_EQ(store.find(at)->voxels[static_cast<std::size_t>(i)], 9);
  }

  // A source that reports no states (any written before 0.56.0) keeps the cached ones.
  source.stored.front().voxelStates.clear();
  (void)store.setVoxel(at, 4, 5, 6, 3, Bytes(local, 1), core::ActorUuid{});
  CHECK_EQ(store.ensureAround(at, 1), 1u);
  CHECK(store.voxelStateAt(at, 4, 5, 6) != nullptr);
  CHECK(store.voxelStateAt(at, 1, 2, 3) != nullptr);
}

int main() {
  testTickDropsRefusalsAndRetriesTheRest();
  testAnInjectedSourceHydratesAndWritesBack();
  testABulkLoadAppliesTheRecordedEdits();
  testALoadTakesTheServersStates();
  testARefusedChunkDoesNotBlockTheOthers();
  testABackingOffChunkDoesNotBlockTheOthers();
  testFlushReportsDroppedWriteBacks();
  testPruneBeyondEvictsRefusedChunks();
  testPruneBeyondSurvivesACallbackThatChangesTheStore();
  std::puts("chunk_store_test OK");
  return 0;
}
