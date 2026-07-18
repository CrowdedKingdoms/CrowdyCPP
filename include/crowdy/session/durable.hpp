#pragma once

#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/core/base64.hpp"
#include "crowdy/core/uuid.hpp"

/// Durable-state stores: the per-user app save blob (SaveStateStore), typed
/// avatar state (AvatarStateStore), and pluggable actor-UUID persistence
/// (IUuidStore). These wrap the GraphQL surfaces with local caches — durable
/// and realtime payloads rarely share a layout, so these carry their own
/// bytes independent of the replication codec.
namespace crowdy::session {

// ---------------------------------------------------------------------------
// Actor-UUID persistence
// ---------------------------------------------------------------------------

/// Where the local actor's persistent identity lives. An actor UUID should
/// survive restarts so other players' registries treat you as the same actor.
class IUuidStore {
 public:
  virtual ~IUuidStore() = default;
  virtual std::optional<core::ActorUuid> load() = 0;
  virtual void save(const core::ActorUuid& uuid) = 0;
};

class MemoryUuidStore final : public IUuidStore {
 public:
  std::optional<core::ActorUuid> load() override { return uuid_; }
  void save(const core::ActorUuid& uuid) override { uuid_ = uuid; }

 private:
  std::optional<core::ActorUuid> uuid_;
};

/// Persists the uuid in a file (the native analog of CrowdyJS's
/// localStorage-backed store).
class FileUuidStore final : public IUuidStore {
 public:
  explicit FileUuidStore(std::string path) : path_(std::move(path)) {}

  std::optional<core::ActorUuid> load() override {
    std::ifstream in(path_);
    std::string line;
    if (!in || !std::getline(in, line)) return std::nullopt;
    core::ActorUuid uuid;
    if (!core::actorUuidFromString(line, uuid)) return std::nullopt;
    return uuid;
  }

  void save(const core::ActorUuid& uuid) override {
    std::ofstream out(path_, std::ios::trunc);
    out << core::toString(uuid);
  }

 private:
  std::string path_;
};

/// Load a persisted uuid or mint + persist a fresh one.
inline core::ActorUuid ensureActorUuid(IUuidStore& store) {
  if (auto existing = store.load()) return *existing;
  core::ActorUuid fresh = core::generateActorUuid();
  store.save(fresh);
  return fresh;
}

// ---------------------------------------------------------------------------
// SaveStateStore — the per-user per-app save blob
// ---------------------------------------------------------------------------

/// Local cache over client.state(): explicit load()/save() with the raw
/// bytes held locally between round trips. The wire form is base64; this
/// store deals in bytes.
class SaveStateStore {
 public:
  SaveStateStore(CrowdyClient& client, std::string appId)
      : client_(client), appId_(std::move(appId)) {}

  /// Fetch the blob from the server into the cache (empty when no row).
  const std::vector<std::uint8_t>& load() {
    graphql::Json row = client_.state().getOne(appId_);
    cache_.clear();
    if (row.ok() && !row.isNull()) {
      if (auto bytes = core::base64Decode(row["state"].asStringView())) cache_ = *bytes;
    }
    loaded_ = true;
    return cache_;
  }

  /// Replace the cached blob and persist it.
  void save(Bytes bytes) {
    cache_.assign(bytes.begin(), bytes.end());
    graphql::JVal input;
    input["appId"] = appId_;
    input["state"] = core::base64Encode(bytes);
    client_.state().update(input);
    loaded_ = true;
  }

  /// Persist the current cache (after in-place edits via value()).
  void save() { save(Bytes(cache_.data(), cache_.size())); }

  /// Overwrite a byte range of the cache and persist (simple patch analog;
  /// grows the cache as needed).
  void patch(std::size_t offset, Bytes bytes) {
    if (cache_.size() < offset + bytes.size()) cache_.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), cache_.begin() + static_cast<std::ptrdiff_t>(offset));
    save();
  }

  bool loaded() const { return loaded_; }
  std::vector<std::uint8_t>& value() { return cache_; }
  const std::vector<std::uint8_t>& value() const { return cache_; }

 private:
  CrowdyClient& client_;
  std::string appId_;
  std::vector<std::uint8_t> cache_;
  bool loaded_ = false;
};

// ---------------------------------------------------------------------------
// AvatarStateStore — typed avatar state (identity + per-app)
// ---------------------------------------------------------------------------

/// Local cache over client.avatars() for one avatar: the identity-level
/// state blob and the per-app state blob, loaded/saved explicitly.
class AvatarStateStore {
 public:
  AvatarStateStore(CrowdyClient& client, std::string appId, std::string avatarId)
      : client_(client), appId_(std::move(appId)), avatarId_(std::move(avatarId)) {}

  const std::string& avatarId() const { return avatarId_; }

  /// Fetch both blobs (identity + app state) into the cache.
  void load() {
    graphql::Json avatar = client_.avatars().get(avatarId_);
    identityState_.clear();
    if (auto bytes = core::base64Decode(avatar["state"].asStringView()))
      identityState_ = *bytes;
    graphql::Json appState = client_.avatars().appState(appId_, avatarId_);
    appState_.clear();
    if (appState.ok() && !appState.isNull()) {
      if (auto bytes = core::base64Decode(appState["state"].asStringView())) appState_ = *bytes;
    }
    loaded_ = true;
  }

  /// Replace + persist the avatar's identity-level state blob.
  void setIdentityState(Bytes bytes) {
    identityState_.assign(bytes.begin(), bytes.end());
    graphql::JVal input;
    input["state"] = core::base64Encode(bytes);
    client_.avatars().updateState(avatarId_, input);
  }

  /// Replace + persist the avatar's per-app state blob.
  void setAppState(Bytes bytes) {
    appState_.assign(bytes.begin(), bytes.end());
    graphql::JVal input;
    input["appId"] = appId_;
    input["avatarId"] = avatarId_;
    input["state"] = core::base64Encode(bytes);
    client_.avatars().updateAppState(input);
  }

  bool loaded() const { return loaded_; }
  const std::vector<std::uint8_t>& identityState() const { return identityState_; }
  const std::vector<std::uint8_t>& appState() const { return appState_; }

 private:
  CrowdyClient& client_;
  std::string appId_;
  std::string avatarId_;
  std::vector<std::uint8_t> identityState_;
  std::vector<std::uint8_t> appState_;
  bool loaded_ = false;
};

}  // namespace crowdy::session
