// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <noir/common/bit.h>
#include <noir/common/check.h>
#include <noir/common/concepts.h>
#include <noir/common/hex.h>
#include <fmt/core.h>

namespace noir {

std::string to_hex(std::span<const char> s) {
  std::string r;
  const char* to_hex = "0123456789abcdef";
  auto c = std::span((const uint8_t*)s.data(), s.size());
  for (const auto& val : c) {
    r += to_hex[val >> 4];
    r += to_hex[val & 0x0f];
  }
  return r;
}

std::string to_hex(uint8_t v) {
  return fmt::format("{:02x}", v);
}

std::string to_hex(uint16_t v) {
  return fmt::format("{:04x}", v);
}

std::string to_hex(uint32_t v) {
  return fmt::format("{:08x}", v);
}

std::string to_hex(uint64_t v) {
  return fmt::format("{:016x}", v);
}

std::string to_hex(uint128_t v) {
  std::string s;
  s += to_hex(uint64_t(v >> 64));
  s += to_hex(uint64_t(v));
  return s;
}

std::string to_hex(uint256_t v) {
  uint64_t data[4] = {0};
  boost::multiprecision::export_bits(v, std::begin(data), 64, false);
  std::string s;
  std::for_each(std::rbegin(data), std::rend(data), [&](auto v) { s += to_hex(v); });
  return s;
}

constexpr uint8_t from_hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  check(false, fmt::format("invalid hex character: {}", c));
  return 0;
}

size_t from_hex(std::string_view s, std::span<char> out) {
  auto has_prefix = s.starts_with("0x");
  auto require_pad = s.size() % 2;
  auto size = (s.size() / 2) + require_pad - has_prefix;
  check(size <= out.size(), "unsufficient output buffer");
  auto c = s.begin() + has_prefix * 2;
  auto r = out.begin();
  for (; c != s.end() && r != out.end(); ++c, ++r) {
    if (!require_pad)
      *r = from_hex(*c++) << 4;
    *r |= from_hex(*c);
    require_pad = false;
  }
  return size;
}

template<integral T>
void _from_hex(std::string_view s, T& v) {
  constexpr auto max_strlen = sizeof(T) * 2;
  v = 0;
  if (s.starts_with("0x")) {
    s = s.substr(2);
  }
  size_t byte_offset = 0;
  auto size = s.size() / 2 + s.size() % 2;
  if (size < sizeof(T)) {
    byte_offset = sizeof(T) - size;
  }
  size_t str_offset = 0;
  if (s.size() > max_strlen) {
    str_offset = s.size() - max_strlen;
  }
  from_hex(s.substr(str_offset), std::span((char*)&v + byte_offset, sizeof(T) - byte_offset));
}

void from_hex(std::string_view s, uint8_t& v) {
  _from_hex(s, v);
}

void from_hex(std::string_view s, uint16_t& v) {
  _from_hex(s, v);
  v = byteswap(v);
}

void from_hex(std::string_view s, uint32_t& v) {
  _from_hex(s, v);
  v = byteswap(v);
}

void from_hex(std::string_view s, uint64_t& v) {
  _from_hex(s, v);
  v = byteswap(v);
}

void from_hex(std::string_view s, uint128_t& v) {
#if defined(__SIZEOF_INT128__)
  auto has_prefix = s.starts_with("0x");
  auto size = 16 + has_prefix * 2;
  if (s.size() > size) {
    uint64_t upper = 0;
    uint64_t lower = 0;
    from_hex(s.substr(0, s.size() - 16), upper);
    v = (uint128_t)upper << 64;
    from_hex(s.substr(s.size() - 16), lower);
    v |= lower;
  } else {
    uint64_t val = 0;
    from_hex(s, val);
    v = val;
  }
#else
  if (!s.starts_with("0x")) {
    v = std::move(uint128_t("0x" + std::string(s)));
  } else {
    v = std::move(uint128_t(s));
  }
#endif
}

void from_hex(std::string_view s, uint256_t& v) {
  if (!s.starts_with("0x")) {
    v = std::move(uint256_t("0x" + std::string(s)));
  } else {
    v = std::move(uint256_t(s));
  }
}

std::vector<unsigned char> from_hex(std::string_view s) {
  auto has_prefix = s.starts_with("0x");
  auto require_pad = s.size() % 2;
  auto size = (s.size() / 2) + require_pad - has_prefix;
  std::vector<unsigned char> out(size);
  auto c = s.begin() + has_prefix * 2;
  auto r = out.begin();
  for (; c != s.end() && r != out.end(); ++c, ++r) {
    if (!require_pad)
      *r = from_hex(*c++) << 4;
    *r |= from_hex(*c);
    require_pad = false;
  }
  return out;
}

} // namespace noir
