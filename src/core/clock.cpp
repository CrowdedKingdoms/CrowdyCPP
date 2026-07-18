#include "crowdy/core/clock.hpp"

#include <chrono>

namespace crowdy::core {

namespace {

class SystemClock final : public IClock {
 public:
  std::int64_t epochMillis() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  std::int64_t monotonicMillis() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

}  // namespace

const IClock& systemClock() {
  static SystemClock clock;
  return clock;
}

}  // namespace crowdy::core
