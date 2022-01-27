// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/common/for_each.h>
#include <fc/crypto/ripemd160.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/io/varint.hpp>

namespace noir {

using ::fc::signed_int;
using ::fc::unsigned_int;

struct bytes20 : fc::ripemd160 {
  using ripemd160::ripemd160;
};

struct bytes32 : fc::sha256 {
  using sha256::sha256;
};

} // namespace noir

NOIR_FOR_EACH_FIELD(fc::ripemd160, _hash);
NOIR_FOR_EACH_FIELD_DERIVED(bytes20, fc::ripemd160);

NOIR_FOR_EACH_FIELD(fc::sha256, _hash);
NOIR_FOR_EACH_FIELD_DERIVED(bytes32, fc::sha256);
