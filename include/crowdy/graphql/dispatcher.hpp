#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

/// Completion pump for the async API layer. Async transports may finish on any
/// thread; posting through a Dispatcher and calling drain() from the game
/// thread makes GraphQL callbacks fire where engine objects are safe to touch.
/// This mirrors the replication layer's pump()/poll() model for the API layer.
namespace crowdy::graphql {

class Dispatcher {
 public:
  /// Enqueue a completion. Thread-safe; callable from any thread.
  void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(fn));
  }

  /// Run and clear every queued callback on the calling thread, returning the
  /// number run. The queue is swapped out before running, so callbacks posted
  /// during a drain are deferred to the next drain rather than run re-entrantly.
  std::size_t drain() {
    std::vector<std::function<void()>> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending.swap(queue_);
    }
    for (auto& fn : pending) fn();
    return pending.size();
  }

 private:
  std::mutex mutex_;
  std::vector<std::function<void()>> queue_;
};

}  // namespace crowdy::graphql
