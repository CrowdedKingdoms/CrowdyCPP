#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "crowdy/core/crypto.hpp"

namespace crowdy::core {

namespace {

class OpensslCrypto final : public ICrypto {
 public:
  bool hmacSha256(Bytes key, Bytes message, std::uint8_t* out) const override {
    unsigned int outLen = 0;
    return HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), message.data(),
                message.size(), out, &outLen) != nullptr &&
           outLen == kHmacTagSize;
  }

  bool sha256(Bytes message, std::uint8_t* out) const override {
    unsigned int outLen = 0;
    return EVP_Digest(message.data(), message.size(), out, &outLen, EVP_sha256(), nullptr) == 1 &&
           outLen == kHmacTagSize;
  }

  bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b,
                          std::size_t len) const override {
    return CRYPTO_memcmp(a, b, len) == 0;
  }

  bool randomBytes(std::uint8_t* out, std::size_t len) const override {
    return RAND_bytes(out, static_cast<int>(len)) == 1;
  }
};

}  // namespace

const ICrypto& opensslCrypto() {
  static OpensslCrypto crypto;
  return crypto;
}

}  // namespace crowdy::core
