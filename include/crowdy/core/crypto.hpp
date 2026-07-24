#pragma once

#include <cstddef>
#include <cstdint>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/result.hpp"

/// Pluggable crypto provider. The SDK needs exactly four primitives:
/// HMAC-SHA256, constant-time comparison, and random bytes. The default
/// implementation (crowdy::core::opensslCrypto()) is backed by OpenSSL's
/// libcrypto; engine wrappers may substitute their own (e.g. an engine's
/// bundled crypto module) by implementing this interface.
namespace crowdy::core {

class ICrypto {
 public:
  virtual ~ICrypto() = default;

  static constexpr std::size_t kHmacTagSize = 32;

  /// out must hold 32 bytes. Returns false on provider failure.
  virtual bool hmacSha256(Bytes key, Bytes message, std::uint8_t* out) const = 0;

  /// Plain SHA-256 (used by the PKCE portal flow). out must hold 32 bytes.
  virtual bool sha256(Bytes message, std::uint8_t* out) const = 0;

  /// Constant-time equality of two 32-byte tags.
  virtual bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b,
                                  std::size_t len) const = 0;

  /// Cryptographically secure random bytes (used for actor UUIDs and PKCE).
  virtual bool randomBytes(std::uint8_t* out, std::size_t len) const = 0;

  /// Provider availability. Injected providers remain source-compatible and
  /// are assumed available unless they override this. The built-in unavailable
  /// provider returns Errc::CryptoUnavailable.
  virtual Status availability() const { return Errc::Ok; }
};

/// Explicit provider used when no platform crypto implementation is linked.
/// Every primitive fails and availability() reports CryptoUnavailable.
const ICrypto& unavailableCrypto();

/// OpenSSL provider. When CROWDY_WITH_OPENSSL is disabled this resolves to the
/// explicit unavailable provider instead of leaving an unresolved symbol;
/// check availability() when provider selection is dynamic.
const ICrypto& opensslCrypto();

/// Build-selected convenience provider: OpenSSL when enabled, otherwise the
/// explicit unavailable provider. Security-sensitive APIs should prefer
/// injected ICrypto ownership so unavailability remains visible to callers.
const ICrypto& defaultCrypto();

}  // namespace crowdy::core
