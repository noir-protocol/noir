#include <catch2/catch_all.hpp>
#include <noir/common/bytes.h>
#include <noir/common/string.h>

using namespace noir;

using Bytes20 = BytesN<20>;
using Bytes32 = BytesN<32>;

TEST_CASE("bytes: variable-length byte sequence", "[noir][common]") {
  SECTION("basic construction") {
    Bytes data({1, 2});
    CHECK(to_string(data) == "0102");
  }

  SECTION("move construction") {
    Bytes from({1, 2});
    void* ptr = from.data();
    Bytes to(std::move(from));
    CHECK(ptr == to.data());
  }
}

TEST_CASE("bytes: fixed-length byte sequence", "[noir][common]") {
  Bytes32 hash{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"};

  SECTION("construction & conversion") {
    // constructs from hex string
    CHECK(hash.to_string() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // constructs from byte sequence
    auto from_span = Bytes32(std::span(hash));
    CHECK(from_span.to_string() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // construct from byte vector
    std::vector<uint8_t> data = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f,
      0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    auto from_vec = Bytes32(data);
    CHECK(from_vec.to_string() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // implicit copy constructor
    auto copied = Bytes32(hash);
    CHECK(copied.to_string() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // implicit move constructor
    auto moved = Bytes32(std::move(Bytes32("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
    CHECK(moved.to_string() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    copied.back() &= 0xf0;
    CHECK(to_string(copied) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b850");
    CHECK(to_string(hash) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // implicit copy assignment operator
    auto copy_assigned = hash;
    CHECK(to_string(copy_assigned) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // converts to variable-length byte sequence
    auto bytes = Bytes{hash};
    CHECK((bytes.size() == hash.size() && hex::encode(bytes.data(), bytes.size()) == to_string(hash)));
  }

  SECTION("comparison") {
    Bytes32 empty{};
    CHECK(empty < hash);
    CHECK(empty.empty());

    auto copied = hash;
    CHECK(copied == hash);
    CHECK(!hash.empty());

    // lexicographical comparison between diffrent sized bytesN
    Bytes20 hash20{"ffffffffffffffffffffffffffffffffffffffff"};
    CHECK(hash20 > hash);
    CHECK(!(hash20 == hash));
    CHECK(hash20 != hash);

    hash20 = {"e3b0c44298fc1c149afbf4c8996fb92427ae41e4"};
    CHECK(hash20 < hash);
    CHECK(hash20 != hash);
  }
}
