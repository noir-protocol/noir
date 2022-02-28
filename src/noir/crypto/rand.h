// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/common/types/bytes.h>
#include <span>

namespace noir::crypto {

/// \brief generates random bytes
/// \ingroup crypto
void rand_bytes(std::span<byte_type> out);

/// \brief generates random bytes using separated PRNG
/// \ingroup crypto
void rand_priv_bytes(std::span<byte_type> out);

} // namespace noir::crypto
