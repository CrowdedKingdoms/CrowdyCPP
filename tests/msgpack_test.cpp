#include <string>

#include "crowdy/graphql/json.hpp"
#include "test_util.hpp"

using namespace crowdy::graphql;

namespace {

std::string hex(const std::string& b) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : b) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0xf]);
  }
  return out;
}

std::string unhex(std::string_view h) {
  std::string out;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2)
    out.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
  return out;
}

std::string pack(std::string_view json) { return hex(Json::parse(json).toMsgpack()); }

/// Encodings a Rust hub (rmp-serde) and CrowdyJS (@msgpack/msgpack) agree on.
void testEncodesTheSmallestForm() {
  CHECK_EQ(pack(R"({"match":"m1","weapon":1})"), "82a56d61746368a26d31a6776561706f6e01");
  CHECK_EQ(pack("null"), "c0");
  CHECK_EQ(pack("[true,false]"), "92c3c2");
  CHECK_EQ(pack("127"), "7f");
  CHECK_EQ(pack("128"), "cc80");
  CHECK_EQ(pack("65535"), "cdffff");
  CHECK_EQ(pack("4294967296"), "cf0000000100000000");
  CHECK_EQ(pack("-1"), "ff");
  CHECK_EQ(pack("-32"), "e0");
  CHECK_EQ(pack("-33"), "d0df");
  CHECK_EQ(pack("-200"), "d1ff38");
  CHECK_EQ(pack("-9223372036854775808"), "d38000000000000000");
  CHECK_EQ(pack("18446744073709551615"), "cfffffffffffffffff");
  CHECK_EQ(pack("1.5"), "cb3ff8000000000000");
  CHECK_EQ(pack(R"("")"), "a0");
  CHECK_EQ(pack(R"("abcdefghijklmnopqrstuvwxyz012345")"),
           "d920" + hex("abcdefghijklmnopqrstuvwxyz012345"));
  CHECK_EQ(Json().toMsgpack(), unhex("c0"));
}

void testRoundTripsKeepIntegersExact() {
  const char* doc = R"({"player":94460401413120,"hp":-5,"ratio":0.25,"tags":["a","b"],"nested":{"ok":true,"none":null}})";
  Json back = Json::fromMsgpack(Json::parse(doc).toMsgpack());
  CHECK(back.ok());
  CHECK_EQ(back["player"].asBigIntString(), "94460401413120");
  CHECK_EQ(back["hp"].asInt64(), -5);
  CHECK_EQ(back["ratio"].asDouble(), 0.25);
  CHECK_EQ(back["tags"].at(1).asString(), "b");
  CHECK(back["nested"]["ok"].asBool());
  CHECK(back["nested"]["none"].isNull());
  CHECK_EQ(back.dump(), Json::parse(doc).dump());

  CHECK_EQ(Json::fromMsgpack(unhex("cfffffffffffffffff")).asBigIntString(), "18446744073709551615");
  CHECK_EQ(Json::fromMsgpack(unhex("ca3fc00000")).asDouble(), 1.5);
}

void testDecodesWhatJsonCannotHold() {
  // Binary becomes base64, extension types null, and non-string keys text.
  CHECK_EQ(Json::fromMsgpack(unhex("c403010203")).asString(), "AQID");
  CHECK(Json::fromMsgpack(unhex("d40105")).isNull());
  Json keyed = Json::fromMsgpack(unhex("810102"));
  CHECK(keyed.ok());
  CHECK_EQ(keyed["1"].asInt64(), 2);
}

void testRefusesMalformedInput() {
  CHECK(!Json::fromMsgpack("").ok());
  CHECK(!Json::fromMsgpack(unhex("c1")).ok());
  CHECK(!Json::fromMsgpack(unhex("92c3")).ok());
  CHECK(!Json::fromMsgpack(unhex("cd01")).ok());
  CHECK(!Json::fromMsgpack(unhex("a3616263ff")).ok());
  std::string deep(70, static_cast<char>(0x91));
  deep.push_back(static_cast<char>(0xc0));
  CHECK(!Json::fromMsgpack(deep).ok());
}

}  // namespace

int main() {
  testEncodesTheSmallestForm();
  testRoundTripsKeepIntegersExact();
  testDecodesWhatJsonCannotHold();
  testRefusesMalformedInput();
  std::puts("msgpack_test OK");
  return 0;
}
