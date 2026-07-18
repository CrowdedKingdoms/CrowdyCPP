#pragma once

#include <cstddef>
#include <cstdint>

#include "crowdy/core/bytes.hpp"

/// Pluggable crypto provider. The SDK needs exactly three primitives:
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
};

/// Default provider (OpenSSL). Only available when the SDK is built with
/// CROWDY_WITH_OPENSSL (the default).
const ICrypto& opensslCrypto();

}  // namespace crowdy::core
