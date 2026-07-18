#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/// Shared auth state observed by the HTTP client and the replication client,
/// so credentials never drift within one CrowdyClient. Mirrors CrowdyJS's
/// AuthState + token store model.
namespace crowdy::graphql {

/// Pluggable token persistence (the CrowdyJS TokenStore analog).
class ITokenStore {
 public:
  virtual ~ITokenStore() = default;
  virtual std::string load() = 0;
  virtual void save(const std::string& token) = 0;
  virtual void clear() = 0;
};

class InMemoryTokenStore final : public ITokenStore {
 public:
  std::string load() override { return token_; }
  void save(const std::string& token) override { token_ = token; }
  void clear() override { token_.clear(); }

 private:
  std::string token_;
};

/// Stores the token in a file (0600). Suitable for native game installs.
class FileTokenStore final : public ITokenStore {
 public:
  explicit FileTokenStore(std::string path) : path_(std::move(path)) {}
  std::string load() override;
  void save(const std::string& token) override;
  void clear() override;

 private:
  std::string path_;
};

class AuthState {
 public:
  explicit AuthState(std::shared_ptr<ITokenStore> store = nullptr)
      : store_(store ? std::move(store) : std::make_shared<InMemoryTokenStore>()) {}

  std::string token() const {
    std::lock_guard lock(mutex_);
    return token_;
  }
  bool hasToken() const {
    std::lock_guard lock(mutex_);
    return !token_.empty();
  }

  void setToken(const std::string& token);
  void clearToken();
  /// Load a previously persisted token from the store.
  bool restore();

  /// Observe token changes (empty string = cleared). Called with the new
  /// token outside the internal lock.
  void onChange(std::function<void(const std::string&)> listener);

 private:
  mutable std::mutex mutex_;
  std::string token_;
  std::shared_ptr<ITokenStore> store_;
  std::vector<std::function<void(const std::string&)>> listeners_;
};

}  // namespace crowdy::graphql
