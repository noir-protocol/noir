// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <catch2/catch_all.hpp>
#include <noir/crypto/rand.h>
#include <span>

using namespace noir;
using namespace noir::crypto;

TEST_CASE("rand", "[noir][crypto]") {
  std::vector<byte_type> out_vec(8);
  std::array<byte_type, 8> out_arr{};

  rand_bytes(out_vec);
  rand_bytes(std::span(out_vec));
  rand_bytes(std::span(out_arr));
}
