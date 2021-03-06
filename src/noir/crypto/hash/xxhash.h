// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/crypto/hash/hash.h>
#include <optional>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace noir::crypto {

/// \brief generates xxh64 hash
/// \ingroup crypto
struct Xxh64 : public Hash<Xxh64> {
  using Hash::update;

  auto init() -> Xxh64&;
  auto update(std::span<const unsigned char> in) -> Xxh64&;
  void final(std::span<unsigned char> out);
  auto final() -> uint64_t;

  constexpr auto digest_size() const -> size_t {
    return 8;
  }

  auto operator()(ByteSequence auto&& in) -> uint64_t {
    return init().update(bytes_view(in)).final();
  }

private:
  std::optional<XXH64_state_t> state;
};

} // namespace noir::crypto
