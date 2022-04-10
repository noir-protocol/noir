// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <catch2/catch_all.hpp>
#include <noir/common/hex.h>
#include <noir/core/codec.h>
#include <tendermint/common/bitarray.h>

using namespace noir;
using namespace tendermint;

TEST_CASE("bitarray", "[noir][consensus]") {
  SECTION("constructors") {
    bitarray ba(10);
    auto bb = ba;
    bb.set(4, true);
    CHECK(bb.size() == 10);
    CHECK(bb.get(4));
    CHECK(ba.get(4) == false);
    auto bc = std::move(bb);
    CHECK(bc.size() == 10);
    CHECK(bc.get(4));
  }

  SECTION("size_words") {
    bitarray ba;
    CHECK(ba.size_words() == 0);
    ba = bitarray(10);
    CHECK(ba.size_words() == 1);
    ba = bitarray(65);
    CHECK(ba.size_words() == 2);
  }

  SECTION("set/get") {
    bitarray ba(65);
    CHECK(ba.set(4, true));
    CHECK(ba.set(64, true));
    CHECK(ba.set(65, true) == false);
    CHECK(ba.data()[0] == (1 << 4));
    CHECK(ba.data()[1] == 1);
    CHECK((ba.get(4) && !ba.get(63) && ba.get(64)));
  }

  SECTION("update") {
    bitarray ba(10);
    bitarray bb(16);
    bb.set(4, true);
    bb.set(12, true);
    CHECK(ba.get(4) == false);
    ba.update(bb);
    CHECK(ba.size() == 10);
    CHECK(ba.get(4));
    CHECK(ba.get(12) == false);
  }

  SECTION("sub") {
    {
      bitarray ba(60);
      bitarray bb(66);
      ba.set(0, true);
      ba.set(1, true);
      bb.set(1, true);
      bb.set(2, true);
      bb.set(64, true);
      ba.sub(bb);
      CHECK(ba.get(0));
      CHECK(ba.get(1) == false);
      CHECK(ba.get(2) == false);
      CHECK(ba.get(3) == false);
    }

    {
      bitarray ba(66);
      bitarray bb(60);
      ba.set(0, true);
      ba.set(1, true);
      ba.set(64, true);
      bb.set(1, true);
      bb.set(2, true);
      ba.sub(bb);
      CHECK(ba.get(0));
      CHECK(ba.get(1) == false);
      CHECK(ba.get(2) == false);
      CHECK(ba.get(3) == false);
      CHECK(ba.get(64));
      CHECK(ba.get(65) == false);
    }
  }

  SECTION("or") {
    {
      bitarray ba(60);
      bitarray bb(66);
      ba.set(0, true);
      ba.set(1, true);
      bb.set(1, true);
      bb.set(2, true);
      bb.set(64, true);
      ba.or_op(bb);
      CHECK(ba.size() == 66);
      CHECK(ba.get(0));
      CHECK(ba.get(1));
      CHECK(ba.get(2));
      CHECK(ba.get(3) == false);
      CHECK(ba.get(64));
    }

    {
      bitarray ba(66);
      bitarray bb(60);
      ba.set(0, true);
      ba.set(1, true);
      ba.set(64, true);
      bb.set(1, true);
      bb.set(2, true);
      ba.or_op(bb);
      CHECK(ba.size() == 66);
      CHECK(ba.get(0));
      CHECK(ba.get(1));
      CHECK(ba.get(2));
      CHECK(ba.get(3) == false);
      CHECK(ba.get(64));
    }
  }

  SECTION("not") {
    bitarray ba(10);
    ba.set(4, true);
    ba.not_op();
    CHECK(ba.get(0));
    CHECK(ba.get(4) == false);
    CHECK(((ba.data()[0] >> 12) & 0b1) == 0);
  }

  SECTION("conversion") {
    bitarray ba(9);
    ba.data()[0] = 257;
    CHECK(ba.to_string() == "x_______x");
    auto bs = ba.to_bytes();
    CHECK((bs[0] == 1 && bs[1] == 1));
    auto encoded = encode(ba);
    CHECK(to_hex(encoded) == "090101");
    auto decoded = decode<bitarray>(encoded);
    CHECK(decoded.size() == 9);
    CHECK(decoded.size_words() == 1);
    CHECK(decoded.data()[0] == 257);
  }
}
