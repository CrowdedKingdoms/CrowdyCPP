// ChunkStore: durable write-back failures (a refusal the server will not change is sent once
// and dropped, a failure that can clear is retried with backoff and then dropped, and neither
// holds up the other chunks), region loads off the game thread, paced realtime edits that are
// undone when refused or unanswered, the compact chunk storage and the per-tick change reports.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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

replication::Connection& unopened() {
  static replication::Connection conn{replication::Config{}, std::make_shared<NoProvider>(),
                                      core::defaultCrypto()};
  return conn;
}

/// A send that always goes out, numbering edits as a connection would.
struct FakeSends {
  std::vector<std::uint8_t> sent;
  std::uint8_t next = 1;
  bool refuse = false;
  ChunkStore::Options install(ChunkStore::Options options) {
    options.send = [this](const ChunkCoord&, const core::ActorUuid&, std::int16_t, std::int16_t,
                          std::int16_t, std::int16_t, Bytes, std::uint8_t) -> Result<std::uint8_t> {
      if (refuse) return Errc::NotConnected;
      sent.push_back(next);
      return next++;
    };
    return options;
  }
};

/// A ChunkStore over the fake Game API, its durable calls run on this thread (runIo).
struct Fixture {
  std::shared_ptr<ChunkApiTransport> http = std::make_shared<ChunkApiTransport>();
  std::shared_ptr<graphql::GraphQLClient> gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
      std::make_shared<graphql::AuthState>());
  domains::ChunksAPI chunksApi{gql};
  std::int64_t clock = 1000;
  std::vector<ChunkWriteBackFailure> reported;
  std::unique_ptr<ChunkStore> store;

  Fixture() {
    ChunkStore::Options options;
    options.writeBackIntervalMs = 100;
    options.ioThread = false;
    options.now = [this] { return clock; };
    store = std::make_unique<ChunkStore>(unopened(), &chunksApi, "42", options);
    store->onWriteBackFailed(
        [this](const ChunkWriteBackFailure& failure) { reported.push_back(failure); });
  }

  /// One frame: queue what is due, run the durable calls, adopt their outcomes.
  void step(std::int64_t now) {
    clock = now;
    store->tick(now);
    store->runIo();
    store->tick(now);
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

std::vector<std::uint8_t> bytesOf(Bytes b) { return {b.begin(), b.end()}; }

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
  for (std::int64_t now = 1000; now <= 30000; now += 100) f.step(now);

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
  for (std::int64_t now = 1000; now < 1000 + 4 * 100; now += 100) f.step(now);
  CHECK_EQ(f.store->pendingWriteBacks(), 0u);
  CHECK_EQ(f.http->calls[1], 1);
  for (std::int64_t x : {2, 3, 4}) CHECK(f.store->find(Fixture::at(x))->storedOnServer);
}

// A chunk waiting out its backoff does not hold up the others either.
void testABackingOffChunkDoesNotBlockTheOthers() {
  Fixture f;
  f.seed(1, Answer::NetworkDown);
  f.step(1000);  // attempt 1 fails; next try due at 1700
  f.seed(2);
  f.step(1100);
  CHECK(f.store->find(Fixture::at(2))->storedOnServer);
  CHECK_EQ(f.http->calls[1], 1);
  f.step(1600);  // chunk 1 is not due yet
  CHECK_EQ(f.http->calls[1], 1);
  f.step(1700);
  CHECK_EQ(f.http->calls[1], 2);
}

// flush() queues every dirty chunk and returns at once; flushAndWait() drives the backoff to the
// end without sleeping the store, and reports what it dropped.
void testFlushQueuesAndFlushAndWaitReportsDroppedWriteBacks() {
  Fixture f;
  f.seed(1, Answer::Forbidden);
  f.seed(2, Answer::BadRequest);
  f.seed(3, Answer::BusyTwice);
  f.seed(4, Answer::NetworkDown);
  f.seed(5);

  CHECK_EQ(f.store->flush().persisted, 5u);  // queued: nothing has run
  CHECK(f.http->calls.empty());
  // The fake clock moves 100 ms each time the store reads it: the 10.5 s of backoff pass quickly.
  ChunkStore::Options options;
  const ChunkFlushResult result = [&] {
    auto& clock = f.clock;
    f.store = nullptr;
    options.writeBackIntervalMs = 100;
    options.ioThread = false;
    options.now = [&clock] { return clock += 100; };
    f.store = std::make_unique<ChunkStore>(unopened(), &f.chunksApi, "42", options);
    f.store->onWriteBackFailed([&f](const ChunkWriteBackFailure& failure) { f.reported.push_back(failure); });
    for (std::int64_t x : {1, 2, 3, 4, 5}) f.store->seed(Fixture::at(x), {});
    return f.store->flushAndWait(60000);
  }();
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
}

// pruneBeyond evicts clean chunks at once; a dirty one stays until its write-back ends (queued by
// the prune), and a later prune evicts it whether it was persisted or dropped.
void testPruneBeyondWaitsForADirtyChunksWriteBack() {
  Fixture f;
  f.seed(0);
  f.step(1000);  // chunk 0 persisted
  f.seed(10, Answer::Forbidden);
  f.seed(11);
  f.store->insertGenerated(Fixture::at(12), {});
  f.step(1050);  // throttled: nothing new goes out
  CHECK_EQ(f.store->pruneBeyond(Fixture::at(0), 2), 0u);  // 10, 11 and 12 are dirty
  CHECK(f.store->find(Fixture::at(10)) != nullptr);
  f.store->runIo();
  f.step(1060);
  CHECK_EQ(f.http->calls[10], 1);
  CHECK(f.reportedFor(10) != nullptr);
  CHECK_EQ(f.store->pruneBeyond(Fixture::at(0), 2), 3u);
  CHECK(f.store->find(Fixture::at(10)) == nullptr);
  CHECK(f.store->find(Fixture::at(11)) == nullptr);
  CHECK(f.store->find(Fixture::at(0)) != nullptr);
}

// A failure callback may change the store (here it caches enough chunks to reuse and add slots)
// while the store adopts write-back outcomes.
void testACallbackThatChangesTheStore() {
  Fixture f;
  f.seed(0);
  f.seed(10, Answer::Forbidden);
  f.store->onWriteBackFailed([&](const ChunkWriteBackFailure&) {
    for (std::int64_t y = 1; y <= 256; ++y) f.store->insertGenerated(ChunkCoord{0, y, 0}, {});
  });
  f.store->flush();
  f.store->runIo();
  f.step(1000);
  CHECK_EQ(f.store->size(), 258u);
  CHECK(f.store->find(Fixture::at(10)) != nullptr && !f.store->find(Fixture::at(10))->dirty);
}

}  // namespace

/// An injected IChunkSource loads and writes back with the same refusal / retry classification
/// the Game API path uses. It records the boxes it was asked for.
class ScriptedSource final : public IChunkSource {
 public:
  std::vector<StoredChunk> stored;
  /// Per chunk x: outcomes handed out in order; once exhausted, the write succeeds.
  std::map<std::int64_t, std::vector<graphql::GraphQLOutcome>> answers;
  std::vector<ChunkCoord> written;
  std::vector<std::pair<ChunkCoord, ChunkCoord>> boxes;
  std::vector<std::string> sinces;
  std::vector<StoredChunkEdits> edits;
  std::vector<ChunkCoord> singles;
  int lastDistance = 0;
  bool viaBoxes = false;

  std::vector<StoredChunk> chunksAround(const std::string& appId, const ChunkCoord&,
                                        int distance) override {
    CHECK_EQ(appId, std::string("42"));
    lastDistance = distance;
    return stored;
  }

  std::vector<StoredChunk> chunksInBox(const std::string& appId, const ChunkCoord& lo,
                                       const ChunkCoord& hi, const std::string& since) override {
    boxes.emplace_back(lo, hi);
    sinces.push_back(since);
    if (!viaBoxes) return IChunkSource::chunksInBox(appId, lo, hi, since);
    std::vector<StoredChunk> out;
    for (const StoredChunk& c : stored) {
      if (c.coord.x >= lo.x && c.coord.x <= hi.x && c.coord.y >= lo.y && c.coord.y <= hi.y &&
          c.coord.z >= lo.z && c.coord.z <= hi.z)
        out.push_back(c);
    }
    return out;
  }

  std::vector<StoredChunkEdits> editsInBox(const std::string&, const ChunkCoord&,
                                           const ChunkCoord&, const std::string&) override {
    return edits;
  }

  std::optional<StoredChunk> chunkAt(const std::string&, const ChunkCoord& coord) override {
    singles.push_back(coord);
    for (const StoredChunk& c : stored)
      if (c.coord == coord) return c;
    return std::nullopt;
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

ChunkStore::Options onThisThread(ChunkStore::Options options = {}) {
  options.ioThread = false;
  return options;
}

void testAnInjectedSourceHydratesAndWritesBack() {
  ScriptedSource source;
  StoredChunk stored;
  stored.coord = {5, 0, 0};
  stored.voxels.assign(kChunkVolume, 9);
  source.stored.push_back(stored);
  source.answers[1] = {graphqlError("FORBIDDEN", true)};
  source.answers[2] = {graphqlError("PLATFORM_BUSY", true)};

  ChunkStore::Options options;
  options.writeBackIntervalMs = 100;
  options.writeBackBackoffMs = 1;
  ChunkStore store(unopened(), &source, "42", options);  // its own I/O thread
  std::vector<ChunkWriteBackFailure> reported;
  store.onWriteBackFailed([&](const ChunkWriteBackFailure& f) { reported.push_back(f); });

  CHECK_EQ(store.ensureAround({0, 0, 0}, 12), 1u);
  CHECK_EQ(source.lastDistance, 8);  // clamped to 0-8, the 17-chunk cube as one box
  CHECK_EQ(store.voxelTypeAt({5, 0, 0}, 1, 2, 3), 9);

  store.seed({1, 0, 0}, {});
  store.seed({2, 0, 0}, {});
  ChunkFlushResult result = store.flushAndWait(10000);
  CHECK_EQ(result.persisted, 1u);  // chunk 2, on its second attempt
  CHECK_EQ(result.dropped.size(), 1u);
  CHECK((result.dropped.front().coord == ChunkCoord{1, 0, 0}));
  CHECK(result.dropped.front().reason == ChunkWriteBackDrop::Refused);
  CHECK_EQ(result.dropped.front().attempts, 1);
  CHECK_EQ(reported.size(), 1u);
  CHECK_EQ(store.pendingWriteBacks(), 0u);

  ChunkStore offline(unopened(), nullptr, "42", options);  // no durable store
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
// `voxels`. The region load selects the entries and puts each over the stored grid.
void testABulkLoadAppliesTheRecordedEdits() {
  auto http = std::make_shared<RecordedChunksTransport>();
  http->response = readFixture("chunks-by-distance-recorded-edits.json");
  auto gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
      std::make_shared<graphql::AuthState>());
  domains::ChunksAPI chunksApi{gql};
  ChunkStore store(unopened(), &chunksApi, "42", ChunkStore::Options{});

  CHECK_EQ(store.ensureAround({2, 1, -1}, 1), 3u);
  const graphql::Json request = graphql::Json::parse(http->lastBody);
  CHECK(request["query"].asString().find("voxelStates") != std::string::npos);
  CHECK_EQ(request["variables"]["input"]["lowChunk"]["x"].asBigInt(), 1);
  CHECK_EQ(request["variables"]["input"]["highChunk"]["z"].asBigInt(), 0);

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
      const auto state = store.voxelStateAt(coord, x, y, z);
      if (entry["state"].isString()) {
        CHECK(state.has_value());
        CHECK_EQ(state->voxelType, entry["voxelType"].asInt64());
        CHECK(bytesOf(state->state) == decoded(entry["state"]));
      } else {
        CHECK(!state.has_value());
      }
      ++entries;
    });
    const ChunkData* cached = store.find(coord);
    CHECK(cached != nullptr && cached->storedOnServer);
    CHECK(std::memcmp(cached->grid(), expected.data(), expected.size()) == 0);
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
  const auto state = store.voxelStateAt(placed, 10, 1, 3);
  CHECK(state.has_value());
  CHECK(std::string(state->state.begin(), state->state.end()) ==
        R"({"actorUuid":"00000000000000000000000000000001"})");
}

replication::SpatialNotification voxelNote(const ChunkCoord& chunk, const char* uuid,
                                           std::uint8_t sequence = 1) {
  return {wire::MessageType::VoxelUpdateNotification, 42, chunk, uuid, Bytes(), 1, sequence};
}

// A load takes the server's copy: it replaces the chunk's states and overlay (a world.set_chunk
// drops the older edits), an entry outside the chunk goes to the overlay (never onto the voxel its
// index would alias), and this client's unanswered edits are put back over it.
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

  FakeSends sends;
  ChunkStore::Options options = onThisThread(sends.install({}));
  options.writeBackIntervalMs = 0;
  ChunkStore store(unopened(), &source, "42", options);
  const ChunkCoord at{5, 0, 0};
  const char other[33] = "0123456789abcdef0123456789abcdef";
  const std::uint8_t merged[] = {0x55};
  store.ingest(voxelNote(at, other), {1, 2, 3, 3, Bytes(merged, 1)});
  store.ingest(voxelNote(at, other), {4, 5, 6, 3, Bytes(merged, 1)});
  CHECK(store.voxelStateAt(at, 4, 5, 6).has_value());

  CHECK_EQ(store.ensureAround(at, 1), 1u);
  CHECK_EQ(store.voxelTypeAt(at, 1, 2, 3), 7);
  const auto kept = store.voxelStateAt(at, 1, 2, 3);
  CHECK(kept.has_value() && kept->voxelType == 7 && bytesOf(kept->state) == std::vector<std::uint8_t>{0xab});
  CHECK_EQ(store.voxelTypeAt(at, 4, 5, 6), 0);
  CHECK(!store.voxelStateAt(at, 4, 5, 6).has_value());
  CHECK_EQ(store.find(at)->stateCount(), 1u);
  CHECK_EQ(store.voxelTypeAt(at, 0, 1, 0), 9);  // where (16,0,0) would land unchecked
  for (int i = 0; i < kChunkVolume; ++i) {
    if (i == voxelIndex(1, 2, 3) || i == voxelIndex(4, 5, 6)) continue;
    CHECK_EQ(store.find(at)->typeAt(i), 9);
  }
  // The two entries outside the chunk are kept, with their states, in the overlay.
  CHECK_EQ(store.find(at)->overlayCount(), 2u);
  CHECK_EQ(store.voxelTypeAt(at, 16, 0, 0), 5);
  CHECK_EQ(store.voxelTypeAt(at, 0, -1, 0), 5);
  const auto far = store.voxelStateAt(at, 16, 0, 0);
  CHECK(far.has_value() && bytesOf(far->state) == std::vector<std::uint8_t>{0x01});
  CHECK_EQ(store.voxelTypeAt(at, 17, 0, 0), 0);

  // A copy without states replaces the states too; this client's unanswered edit stays over it.
  source.stored.front().voxelStates.clear();
  const std::uint8_t local[] = {0x66};
  CHECK(store.setVoxel(at, 4, 5, 6, 3, Bytes(local, 1), core::ActorUuid{}).ok());
  CHECK_EQ(store.ensureAround(at, 1), 0u);  // a loaded chunk is not loaded again
  CHECK_EQ(store.hydrate(at), 1u);
  CHECK(!store.voxelStateAt(at, 1, 2, 3).has_value());
  const auto mine = store.voxelStateAt(at, 4, 5, 6);
  CHECK(mine.has_value() && bytesOf(mine->state) == std::vector<std::uint8_t>{0x66});
  CHECK_EQ(store.find(at)->overlayCount(), 0u);
}

// Voxel positions and types are the app's signed 16-bit values. An edit the one-byte 16^3 grid
// cannot hold goes to the overlay, whole; reads return it; nothing is written at a position
// outside the grid.
void testWideVoxelsGoToTheOverlay() {
  FakeSends sends;
  ChunkStore::Options options = onThisThread(sends.install({}));
  options.writeBackIntervalMs = 0;
  ChunkStore store(unopened(), nullptr, "42", options);
  const ChunkCoord at{0, 0, 0};
  std::array<std::uint8_t, kChunkVolume> grid{};
  grid.fill(9);
  store.insertGenerated(at, grid);
  const char sender[33] = "0123456789abcdef0123456789abcdef";
  const std::uint8_t log[] = {'o', 'a', 'k'};

  // A type above 255 inside the grid: kept whole, the grid holds 0 there, no dense state.
  store.ingest(voxelNote(at, sender), {1, 2, 3, 300, Bytes(log, sizeof(log))});
  CHECK_EQ(store.voxelTypeAt(at, 1, 2, 3), 300);
  const auto wide = store.voxelStateAt(at, 1, 2, 3);
  CHECK(wide.has_value() && wide->voxelType == 300 && wide->state.size() == 3u);
  const ChunkData* c = store.find(at);
  CHECK_EQ(c->typeAt(voxelIndex(1, 2, 3)), 0u);
  CHECK(!c->stateAt(voxelIndex(1, 2, 3)).has_value());
  const auto entry = c->overlayAt(1, 2, 3);
  CHECK(entry.has_value() && entry->x == 1 && entry->y == 2 && entry->z == 3);

  // Positions outside 0-15 and a negative type: kept, and the grid is untouched.
  store.ingest(voxelNote(at, sender), {16, 0, 0, 5, Bytes()});
  store.ingest(voxelNote(at, sender), {0, -1, 0, 4, Bytes()});
  store.ingest(voxelNote(at, sender), {2, 2, 2, -1, Bytes()});
  CHECK_EQ(store.voxelTypeAt(at, 16, 0, 0), 5);
  CHECK_EQ(store.voxelTypeAt(at, 0, -1, 0), 4);
  CHECK_EQ(store.voxelTypeAt(at, 2, 2, 2), -1);
  CHECK_EQ(store.voxelTypeAt(at, 0, 1, 0), 9);
  CHECK(!store.voxelStateAt(at, 16, 0, 0).has_value());  // no state
  CHECK_EQ(store.find(at)->overlayCount(), 4u);
  for (int i = 0; i < kChunkVolume; ++i) {
    if (i == voxelIndex(1, 2, 3) || i == voxelIndex(2, 2, 2)) continue;
    CHECK_EQ(store.find(at)->typeAt(i), 9);
  }

  // An edit the grid can hold takes the voxel back out of the overlay.
  store.ingest(voxelNote(at, sender), {1, 2, 3, 7, Bytes()});
  CHECK_EQ(store.voxelTypeAt(at, 1, 2, 3), 7);
  CHECK_EQ(store.find(at)->overlayCount(), 3u);

  // A local edit follows the same rule; one whose position does not fit in 16 bits is refused
  // before anything changes.
  (void)store.setVoxel(at, 3, 3, 3, 2000, Bytes(), core::ActorUuid{});
  (void)store.setVoxel(at, -5, 40, 3, 6, Bytes(), core::ActorUuid{});
  CHECK_EQ(store.voxelTypeAt(at, 3, 3, 3), 2000);
  CHECK_EQ(store.voxelTypeAt(at, -5, 40, 3), 6);
  CHECK(store.setVoxel(at, 40000, 0, 0, 1, Bytes(), core::ActorUuid{}).error() ==
        Errc::InvalidArgument);
  CHECK_EQ(store.find(at)->overlayCount(), 5u);
}

// A loaded chunk keeps its cache when a later load covers it, a dirty one is never refreshed, and
// a chunk that only held realtime merges is filled.
void testALoadedChunkKeepsItsCache() {
  ScriptedSource source;
  StoredChunk stored;
  stored.coord = {5, 0, 0};
  stored.voxels.assign(kChunkVolume, 9);
  source.stored.push_back(stored);
  ChunkStore::Options options = onThisThread();
  options.writeBackIntervalMs = 100;
  ChunkStore store(unopened(), &source, "42", options);
  const ChunkCoord at{5, 0, 0};
  const char sender[33] = "0123456789abcdef0123456789abcdef";

  CHECK_EQ(store.ensureAround(at, 1), 1u);
  CHECK(store.find(at)->hydratedAtMs > 0);
  CHECK(store.ingest(voxelNote(at, sender), {1, 1, 1, 4, Bytes()}));
  CHECK_EQ(store.ensureAround(at, 1), 0u);
  CHECK_EQ(store.voxelTypeAt(at, 1, 1, 1), 4);

  store.markDirty(at);
  CHECK(store.find(at)->dirty);
  CHECK_EQ(store.hydrate(at), 0u);
  CHECK_EQ(store.voxelTypeAt(at, 1, 1, 1), 4);

  const ChunkCoord merged{9, 0, 0};
  CHECK(store.ingest(voxelNote(merged, sender), {3, 3, 3, 2, Bytes()}));
  CHECK_EQ(store.find(merged)->hydratedAtMs, 0);
  source.stored.front().coord = merged;
  CHECK_EQ(store.ensureAround(merged, 1), 1u);
  CHECK_EQ(store.voxelTypeAt(merged, 0, 0, 0), 9);
  CHECK(store.find(merged)->storedOnServer);
}

std::string storedChunkJson(std::int64_t x, bool truncated) {
  const std::vector<std::uint8_t> grid(kChunkVolume, 9);
  return R"({"coordinates":{"x":")" + std::to_string(x) + R"(","y":"0","z":"0"},"voxels":")" +
         core::base64Encode(Bytes(grid.data(), grid.size())) +
         R"(","voxelStates":[{"voxelCoord":{"x":1,"y":1,"z":1},"voxelType":3,"state":null}],)" +
         R"("voxelStatesTruncated":)" + (truncated ? "true" : "false") +
         R"(,"updatedAt":"2026-11-11T20:00:00.000Z"})";
}

/// Answers getChunksByDistance from `pages`, keyed by the request's skip, echoing `pageLimit`, and
/// getChunk with `single`. Keeps every request body.
class PagedChunksTransport final : public graphql::IHttpTransport {
 public:
  std::int64_t pageLimit = 2;
  std::map<std::int64_t, std::string> pages;
  std::string single = "null";
  std::vector<std::string> bodies;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    return sendOutcome(request).response;
  }

  graphql::HttpOutcome sendOutcome(const graphql::HttpRequest& request) noexcept override {
    bodies.push_back(request.body);
    const graphql::Json body = graphql::Json::parse(request.body);
    if (body["query"].asStringView().find("getChunksByDistance") != std::string_view::npos) {
      const std::int64_t skip = body["variables"]["input"]["skip"].asInt64(0);
      const auto page = pages.find(skip);
      return {Errc::Ok,
              {200, R"({"data":{"getChunksByDistance":{"limit":)" + std::to_string(pageLimit) +
                        R"(,"skip":)" + std::to_string(skip) + R"(,"chunks":)" +
                        (page == pages.end() ? std::string("[]") : page->second) + "}}}"},
              {}};
    }
    return {Errc::Ok, {200, R"({"data":{"getChunk":)" + single + "}}"}, {}};
  }
};

// The Game API's source reads a box (ck-api v2.67.0) in pages of the size the server says it
// applied; a chunk two pages return is applied once and counted. hydrate() reads one chunk with
// getChunk. voxelStatesTruncated and the stored row's updatedAt reach the cache.
void testTheGameApiSourcePagesABoxAndReadsOneChunk() {
  auto http = std::make_shared<PagedChunksTransport>();
  http->pages[0] = "[" + storedChunkJson(1, false) + "," + storedChunkJson(2, false) + "]";
  http->pages[2] = "[" + storedChunkJson(2, false) + "," + storedChunkJson(3, true) + "]";
  auto gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
      std::make_shared<graphql::AuthState>());
  domains::ChunksAPI chunksApi{gql};
  ChunkStore store(unopened(), &chunksApi, "42", ChunkStore::Options{});

  CHECK_EQ(store.ensureAround({2, 0, 0}, 1), 3u);
  CHECK_EQ(http->bodies.size(), 3u);
  const graphql::Json first = graphql::Json::parse(http->bodies[0]);
  CHECK_EQ(first["variables"]["input"]["limit"].asInt64(), 1000);
  CHECK_EQ(first["variables"]["input"]["skip"].asInt64(), 0);
  CHECK_EQ(first["variables"]["input"]["lowChunk"]["x"].asBigInt(), 1);
  CHECK_EQ(first["variables"]["input"]["highChunk"]["x"].asBigInt(), 3);
  CHECK(first["variables"]["input"]["centerCoordinate"].isNull());
  const graphql::Json second = graphql::Json::parse(http->bodies[1]);
  CHECK_EQ(second["variables"]["input"]["skip"].asInt64(), 2);
  CHECK_EQ(store.duplicatePageChunks(), 1u);
  CHECK(store.find({3, 0, 0})->voxelStatesTruncated);
  CHECK(!store.find({1, 0, 0})->voxelStatesTruncated);
  CHECK_EQ(store.find({1, 0, 0})->storedAtMs, core::parseIso8601Millis("2026-11-11T20:00:00.000Z", 24));
  CHECK_EQ(store.voxelTypeAt({2, 0, 0}, 1, 1, 1), 3);

  http->bodies.clear();
  http->single = storedChunkJson(7, false);
  CHECK_EQ(store.hydrate({7, 0, 0}), 1u);
  CHECK_EQ(http->bodies.size(), 1u);
  const std::string query = graphql::Json::parse(http->bodies[0])["query"].asString();
  CHECK(query.find("getChunk(") != std::string::npos);
  CHECK(query.find("getChunksByDistance") == std::string::npos);
  CHECK_EQ(store.voxelTypeAt({7, 0, 0}, 0, 0, 0), 9);
  CHECK_EQ(store.voxelTypeAt({7, 0, 0}, 1, 1, 1), 3);

  http->single = "null";
  CHECK_EQ(store.hydrate({8, 0, 0}), 0u);
  CHECK(store.find({8, 0, 0}) == nullptr);
}

// C4: a step asks only for what it uncovers, as boxes; the chunks the server does not have are not
// asked for again until they are pruned.
void testAStepReadsOnlyTheSlabItUncovers() {
  ScriptedSource source;
  source.viaBoxes = true;
  ChunkStore store(unopened(), &source, "42", onThisThread());
  CHECK_EQ(store.ensureAroundAsync({0, 0, 0}, 8), static_cast<std::size_t>(17 * 17 * 17));
  store.runIo();
  store.tick(0);
  CHECK_EQ(source.boxes.size(), 1u);
  CHECK((source.boxes[0].first == ChunkCoord{-8, -8, -8} && source.boxes[0].second == ChunkCoord{8, 8, 8}));
  CHECK_EQ(store.loadsInFlight(), 0u);

  CHECK_EQ(store.ensureAroundAsync({1, 0, 0}, 8), 289u);
  store.runIo();
  store.tick(0);
  CHECK_EQ(source.boxes.size(), 2u);
  CHECK((source.boxes[1].first == ChunkCoord{9, -8, -8} && source.boxes[1].second == ChunkCoord{9, 8, 8}));

  // A diagonal step: one box per slab, together exactly what it uncovered.
  CHECK_EQ(store.ensureAroundAsync({2, 0, 1}, 8), 561u);
  store.runIo();
  std::int64_t read = 0;
  for (std::size_t i = 2; i < source.boxes.size(); ++i) {
    const auto& [lo, hi] = source.boxes[i];
    read += (hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
  }
  CHECK_EQ(source.boxes.size(), 4u);
  CHECK_EQ(read, 561);
  // Nothing is stored there, so asking again asks for nothing.
  store.tick(0);
  CHECK_EQ(store.ensureAroundAsync({2, 0, 1}, 8), 0u);
}

// C3: a load runs on the store's I/O thread and tick() applies it within its budget, a page at a
// time; the game thread never waits for the source.
void testALoadIsAppliedWithinTheTickBudget() {
  ScriptedSource source;
  source.viaBoxes = true;
  for (std::int64_t x = -8; x <= 8; ++x)
    for (std::int64_t y = -8; y <= 8; ++y)
      for (std::int64_t z = -8; z <= 8; ++z) {
        StoredChunk c;
        c.coord = {x, y, z};
        c.voxels.assign(kChunkVolume, static_cast<std::uint8_t>((x + y + z) & 7));
        c.voxels[0] = 1;
        for (int i = 0; i < 16; ++i) c.voxelStates.push_back({i, 0, 0, 2, {1, 2, 3, 4}});
        source.stored.push_back(std::move(c));
      }
  ChunkStore::Options options;
  options.adoptBudgetUs = 200;
  ChunkStore store(unopened(), &source, "42", options);
  std::size_t reports = 0;
  store.onChanges([&](const std::vector<ChunkChange>& changes) {
    ++reports;
    for (const ChunkChange& c : changes) CHECK((c.flags & kChunkLoaded) != 0);
  });
  CHECK_EQ(store.ensureAroundAsync({0, 0, 0}, 8), 4913u);
  int ticks = 0;
  while (store.loadsInFlight() > 0 && ticks < 100000) {
    store.tick(ticks++);
    std::this_thread::yield();
  }
  CHECK_EQ(store.size(), 4913u);
  CHECK(ticks > 1);  // not applied in one tick
  CHECK(reports > 1);
}

// C6: a uniform chunk keeps one type, its grid reads without a copy; one edit materializes it.
void testAUniformChunkKeepsOneType() {
  ChunkStore store(unopened(), nullptr, "42", onThisThread());
  std::array<std::uint8_t, kChunkVolume> air{};
  const ChunkData& c = store.insertGenerated({0, 0, 0}, air);
  CHECK(c.uniform());
  CHECK_EQ(c.uniformType(), 0u);
  CHECK(c.memoryBytes() < 512u);
  CHECK_EQ(c.grid()[4095], 0u);
  std::array<std::uint8_t, kChunkVolume> stone{};
  stone.fill(3);
  CHECK(store.insertGenerated({1, 0, 0}, stone).uniform());
  CHECK_EQ(store.find({1, 0, 0})->grid()[17], 3u);
  const char sender[33] = "0123456789abcdef0123456789abcdef";
  store.ingest(voxelNote({0, 0, 0}, sender), {2, 3, 4, 5, Bytes()});
  CHECK(!store.find({0, 0, 0})->uniform());
  CHECK_EQ(store.find({0, 0, 0})->grid()[voxelIndex(2, 3, 4)], 5u);
  CHECK_EQ(store.find({0, 0, 0})->grid()[0], 0u);
}

// C6: every voxel state lives in one buffer per chunk; a state that grows moves, one that shrinks
// stays, and the buffer compacts when most of it is dead.
void testStatesShareOneBuffer() {
  ChunkStore store(unopened(), nullptr, "42", onThisThread());
  store.insertGenerated({0, 0, 0}, {});
  const char sender[33] = "0123456789abcdef0123456789abcdef";
  std::vector<std::uint8_t> big(600, 7);
  for (int i = 0; i < 64; ++i) {
    store.ingest(voxelNote({0, 0, 0}, sender), {i % 16, i / 16, 0, 1, Bytes(big.data(), big.size())});
  }
  CHECK_EQ(store.find({0, 0, 0})->stateCount(), 64u);
  const std::uint8_t small[] = {9};
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 64; ++i) store.ingest(voxelNote({0, 0, 0}, sender), {i % 16, i / 16, 0, 1, Bytes(small, 1)});
    for (int i = 0; i < 64; ++i)
      store.ingest(voxelNote({0, 0, 0}, sender), {i % 16, i / 16, 0, 1, Bytes(big.data(), big.size())});
  }
  const ChunkData* c = store.find({0, 0, 0});
  CHECK_EQ(c->stateCount(), 64u);
  CHECK(c->memoryBytes() < 4u * 64u * 600u);  // compacted, not one buffer per write
  int seen = 0;
  c->forEachState([&](int index, const VoxelStateView& s) {
    CHECK_EQ(s.state.size(), 600u);
    CHECK_EQ(s.state[599], 7u);
    CHECK(index >= 0);
    ++seen;
  });
  CHECK_EQ(seen, 64);
}

// C6: changes come once per tick, coalesced per chunk, with what changed; an edit on a border flags
// the neighbour on that face.
void testChangesAreReportedOncePerTick() {
  ChunkStore store(unopened(), nullptr, "42", onThisThread());
  store.insertGenerated({0, 0, 0}, {});
  store.insertGenerated({-1, 0, 0}, {});
  store.insertGenerated({0, 0, 1}, {});
  std::vector<std::vector<ChunkChange>> reports;
  store.onChanges([&](const std::vector<ChunkChange>& changes) { reports.push_back(changes); });
  store.tick(0);
  CHECK_EQ(reports.size(), 1u);  // the three inserts, in one report
  CHECK_EQ(reports[0].size(), 3u);
  reports.clear();

  const char sender[33] = "0123456789abcdef0123456789abcdef";
  store.ingest(voxelNote({0, 0, 0}, sender), {0, 5, 15, 4, Bytes()});  // on the -x and +z faces
  store.ingest(voxelNote({0, 0, 0}, sender), {5, 5, 5, 4, Bytes()});
  const std::uint8_t s[] = {1};
  store.ingest(voxelNote({0, 0, 0}, sender), {5, 5, 5, 4, Bytes(s, 1)});  // state only
  store.tick(1);
  CHECK_EQ(reports.size(), 1u);
  std::map<std::int64_t, std::uint32_t> byKey;
  for (const ChunkChange& c : reports[0]) byKey[c.coord.x * 10 + c.coord.z] = c.flags;
  CHECK_EQ(byKey.size(), 3u);
  CHECK_EQ(byKey[0], static_cast<std::uint32_t>(kChunkVoxels | kChunkStates));
  CHECK_EQ(byKey[-10], static_cast<std::uint32_t>(kChunkNeighborEdge | kChunkFacePosX));
  CHECK_EQ(byKey[1], static_cast<std::uint32_t>(kChunkNeighborEdge | kChunkFaceNegZ));
  store.tick(2);
  CHECK_EQ(reports.size(), 1u);  // nothing new, no report

  CHECK_EQ(store.pruneBeyond({10, 0, 0}, 1), 3u);
  store.tick(3);
  CHECK_EQ(reports.size(), 2u);
  CHECK_EQ(reports[1].size(), 3u);
  for (const ChunkChange& c : reports[1]) CHECK_EQ(c.flags, static_cast<std::uint32_t>(kChunkUnloaded));
}

// C5: an edit the server refuses is undone and reported; a newer edit of the voxel undoes to what
// the voxel held before both; another client's edit stands.
void testARefusedEditIsUndone() {
  FakeSends sends;
  ChunkStore store(unopened(), nullptr, "42", onThisThread(sends.install({})));
  store.insertGenerated({0, 0, 0}, {});
  std::vector<EditOutcome> refused;
  store.onEditRefused([&](const EditOutcome& o) { refused.push_back(o); });
  std::vector<EditOutcome> done;
  auto id = store.setVoxel({0, 0, 0}, 1, 1, 1, 5, Bytes(), core::ActorUuid{}, true,
                           [&](const EditOutcome& o) { done.push_back(o); });
  CHECK(id.ok());
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 1, 1, 1), 5);
  CHECK_EQ(store.editsInFlight(), 1u);
  CHECK(store.ingestError({sends.sent[0], wire::ErrorCode::Unauthorized}));
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 1, 1, 1), 0);
  CHECK_EQ(refused.size(), 1u);
  CHECK(refused[0].result == EditResult::Refused && refused[0].restored);
  CHECK(refused[0].errorCode == wire::ErrorCode::Unauthorized);
  CHECK_EQ(done.size(), 1u);
  CHECK_EQ(done[0].editId, id.value());
  CHECK_EQ(store.editsInFlight(), 0u);

  (void)store.setVoxel({0, 0, 0}, 2, 2, 2, 3, Bytes(), core::ActorUuid{});
  (void)store.setVoxel({0, 0, 0}, 2, 2, 2, 4, Bytes(), core::ActorUuid{});
  CHECK(store.ingestError({sends.sent[1], wire::ErrorCode::Unauthorized}));
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 2, 2, 2), 4);
  CHECK(!refused.back().restored);
  CHECK(store.ingestError({sends.sent[2], wire::ErrorCode::Unauthorized}));
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 2, 2, 2), 0);

  const char other[33] = "0123456789abcdef0123456789abcdef";
  (void)store.setVoxel({0, 0, 0}, 3, 3, 3, 8, Bytes(), core::ActorUuid{});
  store.ingest(voxelNote({0, 0, 0}, other, 200), {3, 3, 3, 9, Bytes()});
  CHECK(store.ingestError({sends.sent[3], wire::ErrorCode::Unauthorized}));
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 3, 3, 3), 9);
  CHECK(!refused.back().restored);
  CHECK(!store.ingestError({250, wire::ErrorCode::Unauthorized}));  // nobody's
}

// C5: an edit whose echo does not come back in time is undone; an echoed one is kept and fires its
// callback; one the connection does not take is undone at once.
void testUnansweredAndUnsentEditsAreUndone() {
  FakeSends sends;
  std::int64_t now = 0;
  ChunkStore::Options options = onThisThread(sends.install({}));
  options.now = [&] { return now; };
  options.editTimeoutMs = 1000;
  ChunkStore store(unopened(), nullptr, "42", options);
  store.insertGenerated({0, 0, 0}, {});
  std::vector<EditOutcome> refused;
  store.onEditRefused([&](const EditOutcome& o) { refused.push_back(o); });
  bool echoed = false;
  const core::ActorUuid me{};
  (void)store.setVoxel({0, 0, 0}, 1, 1, 1, 5, Bytes(), me);
  (void)store.setVoxel({0, 0, 0}, 2, 2, 2, 6, Bytes(), me, true,
                       [&](const EditOutcome& o) { echoed = o.result == EditResult::Echoed; });
  replication::SpatialNotification echo{wire::MessageType::VoxelUpdateNotification, 42,
                                        {0, 0, 0}, reinterpret_cast<const char*>(me.data()), Bytes(),
                                        1, sends.sent[1]};
  CHECK(!store.ingest(echo, {2, 2, 2, 6, Bytes()}));  // the echo of an applied edit
  CHECK(echoed);
  now = 1100;
  store.tick(now);
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 1, 1, 1), 0);
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 2, 2, 2), 6);
  CHECK_EQ(refused.size(), 1u);
  CHECK(refused[0].result == EditResult::TimedOut);

  sends.refuse = true;
  (void)store.setVoxel({0, 0, 0}, 3, 3, 3, 7, Bytes(), me);
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 3, 3, 3), 0);
  CHECK(refused.back().result == EditResult::NotSent);
}

// C5: at most maxEditsInFlight are unanswered and maxEditsPerSecond go out a second; the rest wait
// in order, already showing.
void testEditsArePaced() {
  FakeSends sends;
  std::int64_t now = 0;
  ChunkStore::Options options = onThisThread(sends.install({}));
  options.now = [&] { return now; };
  options.maxEditsInFlight = 2;
  options.maxEditsPerSecond = 1000;
  ChunkStore store(unopened(), nullptr, "42", options);
  store.insertGenerated({0, 0, 0}, {});
  const core::ActorUuid me{};
  for (int i = 0; i < 4; ++i) (void)store.setVoxel({0, 0, 0}, i, 0, 0, 1, Bytes(), me);
  CHECK_EQ(sends.sent.size(), 2u);
  CHECK_EQ(store.queuedEdits(), 2u);
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 3, 0, 0), 1);
  replication::SpatialNotification echo{wire::MessageType::VoxelUpdateNotification, 42,
                                        {0, 0, 0}, reinterpret_cast<const char*>(me.data()), Bytes(),
                                        1, sends.sent[0]};
  store.ingest(echo, {0, 0, 0, 1, Bytes()});
  CHECK_EQ(sends.sent.size(), 3u);
  CHECK(store.ingestError({sends.sent[1], wire::ErrorCode::Unauthorized}));
  CHECK_EQ(sends.sent.size(), 4u);

  FakeSends slow;
  ChunkStore::Options paced = onThisThread(slow.install({}));
  paced.now = [&] { return now; };
  paced.maxEditsPerSecond = 8;  // two at once, then one each 125 ms
  ChunkStore pacedStore(unopened(), nullptr, "42", paced);
  for (int i = 0; i < 5; ++i) (void)pacedStore.setVoxel({0, 0, 0}, i, 1, 0, 1, Bytes(), me);
  CHECK_EQ(slow.sent.size(), 2u);
  now += 130;
  pacedStore.tick(now);
  CHECK_EQ(slow.sent.size(), 3u);
  now += 260;
  pacedStore.tick(now);
  CHECK_EQ(slow.sent.size(), 5u);
}

// A chunkChanged reads the chunk again (once, deduped) unless the store's copy is that new, and the
// server's copy replaces a seed's queued write-back.
void testAChunkChangedReloadsTheChunk() {
  ScriptedSource source;
  StoredChunk stored;
  stored.coord = {1, 0, 0};
  stored.voxels.assign(kChunkVolume, 4);
  stored.updatedAtMs = 5000;
  source.stored.push_back(stored);
  ChunkStore::Options options = onThisThread();
  options.writeBackIntervalMs = 100;
  ChunkStore store(unopened(), &source, "42", options);
  store.insertGenerated({1, 0, 0}, {});
  CHECK_EQ(store.pendingWriteBacks(), 1u);
  store.ingestChunkChanged({1, 0, 0}, 5000);
  store.ingestChunkChanged({1, 0, 0}, 5000);
  store.runIo();
  store.tick(0);
  store.runIo();
  store.tick(0);
  CHECK_EQ(store.pendingWriteBacks(), 0u);
  CHECK(source.written.empty());
  CHECK_EQ(store.voxelTypeAt({1, 0, 0}, 0, 0, 0), 4);
  CHECK_EQ(store.find({1, 0, 0})->storedAtMs, 5000);
  const std::size_t reads = source.singles.size();
  CHECK(reads >= 1u && reads <= 2u);
  store.ingestChunkChanged({1, 0, 0}, 5000);  // already that new
  store.ingestChunkChanged({40, 0, 0}, 9000);  // not held
  store.runIo();
  CHECK_EQ(source.singles.size(), reads);
}

// After the connection comes back, the store reads what changed in its last area since 30 s before
// it dropped: the recorded edits go over the chunks it holds, and rewritten chunks are applied.
void testAReconnectCatchesUp() {
  ScriptedSource source;
  source.viaBoxes = true;
  StoredChunk a;
  a.coord = {0, 0, 0};
  a.voxels.assign(kChunkVolume, 1);
  a.updatedAtMs = 1000;
  source.stored.push_back(a);
  ChunkStore store(unopened(), &source, "42", onThisThread());
  CHECK_EQ(store.ensureAround({0, 0, 0}, 1), 1u);
  store.connectionState(replication::ConnState::Connected);
  store.connectionState(replication::ConnState::Reconnecting);
  // Meanwhile another player placed a block, and a hub rewrote the chunk.
  source.edits.push_back({{0, 0, 0}, {{4, 4, 4, 3, {}}}});
  source.stored[0].voxels.assign(kChunkVolume, 2);
  source.stored[0].updatedAtMs = 2000;
  store.connectionState(replication::ConnState::Connected);
  store.runIo();
  store.tick(0);
  CHECK_EQ(source.sinces.back().size(), 24u);  // an ISO time
  CHECK_EQ(store.voxelTypeAt({0, 0, 0}, 0, 0, 0), 2);
  CHECK_EQ(store.find({0, 0, 0})->storedAtMs, 2000);
}

int main() {
  testTickDropsRefusalsAndRetriesTheRest();
  testWideVoxelsGoToTheOverlay();
  testAnInjectedSourceHydratesAndWritesBack();
  testABulkLoadAppliesTheRecordedEdits();
  testALoadTakesTheServersStates();
  testALoadedChunkKeepsItsCache();
  testTheGameApiSourcePagesABoxAndReadsOneChunk();
  testARefusedChunkDoesNotBlockTheOthers();
  testABackingOffChunkDoesNotBlockTheOthers();
  testFlushQueuesAndFlushAndWaitReportsDroppedWriteBacks();
  testPruneBeyondWaitsForADirtyChunksWriteBack();
  testACallbackThatChangesTheStore();
  testAStepReadsOnlyTheSlabItUncovers();
  testALoadIsAppliedWithinTheTickBudget();
  testAUniformChunkKeepsOneType();
  testStatesShareOneBuffer();
  testChangesAreReportedOncePerTick();
  testARefusedEditIsUndone();
  testUnansweredAndUnsentEditsAreUndone();
  testEditsArePaced();
  testAChunkChangedReloadsTheChunk();
  testAReconnectCatchesUp();
  std::puts("chunk_store_test OK");
  return 0;
}
