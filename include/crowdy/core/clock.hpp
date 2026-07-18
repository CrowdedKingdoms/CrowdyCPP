#pragma once

#include <cstdint>

/// Pluggable clock so engine wrappers can drive SDK timing from engine time
/// (and tests can use a fake clock).
namespace crowdy::core {

class IClock {
 public:
  virtual ~IClock() = default;
  /// Wall-clock milliseconds since the Unix epoch.
  virtual std::int64_t epochMillis() const = 0;
  /// Monotonic milliseconds (arbitrary origin) for intervals and timeouts.
  virtual std::int64_t monotonicMillis() const = 0;
};

const IClock& systemClock();

}  // namespace crowdy::core
