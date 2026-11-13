#include <chrono>
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

// 0.83.0: a container key was written as escaped text, again at every level a key nests in a
// key, so about 130 bytes nested 60 deep asked for gigabytes. Only scalar keys are kept now.
void testRefusesContainerKeys() {
  std::string nested = unhex("a161");
  for (int i = 0; i < 60; ++i) nested = "\x81" + nested + "\xc0";
  CHECK(nested.size() < 140);
  const auto start = std::chrono::steady_clock::now();
  CHECK(!Json::fromMsgpack(nested).ok());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));

  CHECK(!Json::fromMsgpack(unhex("81910102")).ok());    // [1]: 2
  CHECK(!Json::fromMsgpack(unhex("8180c002")).ok());    // {}: 2
  CHECK(!Json::fromMsgpack(unhex("81c4014102")).ok());  // bin: 2
  CHECK(!Json::fromMsgpack(unhex("81d4010502")).ok());  // fixext: 2
  CHECK(!Json::fromMsgpack(unhex("91819101c0")).ok());  // [{[1]: nil}]: a refused key in an array
  CHECK(Json::fromMsgpack(unhex("91810191c0")).ok());   // [{1: [nil]}]: a scalar key, kept

  CHECK_EQ(Json::fromMsgpack(unhex("81c302"))["true"].asInt64(), 2);
  CHECK_EQ(Json::fromMsgpack(unhex("81c002"))["null"].asInt64(), 2);
  CHECK_EQ(Json::fromMsgpack(unhex("81ff02"))["-1"].asInt64(), 2);
  CHECK_EQ(Json::fromMsgpack(unhex("81ca3fc0000002"))["1.5"].asInt64(), 2);
}

// 0.83.0: dump() returned "null" for a whole value holding one NaN, which yyjson refuses to
// write. NaN and infinity are written as null in place; tryDump() reports what still fails.
void testDumpKeepsTheRestOfAValue() {
  const Json nan = Json::fromMsgpack(unhex("83a161cb7ff8000000000000a162cb7ff0000000000000a16301"));
  CHECK(nan.ok());
  CHECK_EQ(nan.dump(), R"({"a":null,"b":null,"c":1})");
  CHECK(nan.tryDump().ok());

  const Json badText = Json::fromMsgpack(unhex("81a161a1ff"));
  CHECK(badText.ok());
  CHECK_EQ(badText.dump(), "null");
  CHECK(!badText.tryDump().ok());
  CHECK(badText.tryDump().error() == crowdy::Errc::Malformed);
  CHECK_EQ(Json().tryDump().value(), "null");
}

// 0.79.0: encoding used one stack frame per nesting level, so a forged answer a few hundred KB of
// `[[[[` deep overflowed the calling thread's stack. Nesting costs heap now, at any depth.
void testEncodesAnyDepthWithoutTheStack() {
  CHECK_EQ(pack(R"({"k":[1,[2,[]],{}],"z":{"y":null}})"), "82a16b930192029080a17a81a179c0");
  CHECK_EQ(pack(R"([[],{},[[{}]]])"), "939080919180");

  constexpr std::size_t kDepth = 400'000;
  std::string arrays(kDepth, '[');
  arrays.append(kDepth, ']');
  const Json nested = Json::parse(arrays);
  CHECK(nested.ok());
  const std::string packed = nested.toMsgpack();
  CHECK_EQ(packed.size(), kDepth);
  std::size_t singletons = 0;
  for (std::size_t i = 0; i + 1 < packed.size(); ++i) {
    if (static_cast<unsigned char>(packed[i]) == 0x91) ++singletons;
  }
  CHECK_EQ(singletons, kDepth - 1);
  CHECK_EQ(static_cast<unsigned char>(packed.back()), 0x90u);

  std::string objects;
  for (std::size_t i = 0; i < kDepth; ++i) objects += R"({"a":)";
  objects += "1";
  objects.append(kDepth, '}');
  const std::string packedObjects = Json::parse(objects).toMsgpack();
  CHECK_EQ(packedObjects.size(), 3 * kDepth + 1);  // 81 a1 61 per level, then 01
  CHECK_EQ(static_cast<unsigned char>(packedObjects.back()), 0x01u);
}

}  // namespace

int main() {
  testEncodesTheSmallestForm();
  testRoundTripsKeepIntegersExact();
  testDecodesWhatJsonCannotHold();
  testRefusesMalformedInput();
  testRefusesContainerKeys();
  testDumpKeepsTheRestOfAValue();
  testEncodesAnyDepthWithoutTheStack();
  std::puts("msgpack_test OK");
  return 0;
}
