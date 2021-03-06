// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/codec/datastream.h>
#include <noir/common/check.h>
#include <noir/common/concepts.h>
#include <noir/common/for_each.h>
#include <noir/common/pow.h>
#include <noir/common/types.h>
#include <boost/pfr.hpp>
#include <optional>
#include <variant>

NOIR_CODEC(scale) {

// Fixed-width integers
// Boolean
template<typename Stream, integral T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const T& v) {
  ds.write((const unsigned char*)&v, sizeof(v));
  return ds;
}

template<typename Stream, integral T>
datastream<Stream>& operator>>(datastream<Stream>& ds, T& v) {
  ds.read((unsigned char*)&v, sizeof(v));
  return ds;
}

template<typename Stream>
datastream<Stream>& operator<<(datastream<Stream>& ds, const uint256_t& v) {
  uint64_t data[4] = {0};
  boost::multiprecision::export_bits(v, std::begin(data), 64, false);
  ds.write((const unsigned char*)data, 32);
  return ds;
}

template<typename Stream>
datastream<Stream>& operator>>(datastream<Stream>& ds, uint256_t& v) {
  uint64_t data[4] = {0};
  ds.read((unsigned char*)data, 32);
  boost::multiprecision::import_bits(v, std::begin(data), std::end(data), 64, false);
  return ds;
}

template<typename Stream, enumeration E>
datastream<Stream>& operator<<(datastream<Stream>& ds, const E& v) {
  ds << static_cast<const std::underlying_type_t<E>>(v);
  return ds;
}

template<typename Stream, enumeration E>
datastream<Stream>& operator>>(datastream<Stream>& ds, E& v) {
  std::underlying_type_t<E> tmp;
  ds >> tmp;
  v = static_cast<E>(tmp);
  return ds;
}

// Compact/general integers
template<typename Stream, UnsignedIntegral T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const Varint<T>& v) {
  using noir::pown;
  if (v < pown(2u, 6)) {
    ds.put(v.value << 2);
  } else if (v < pown(2u, 14)) {
    uint16_t tmp = v.value << 2 | 0b01;
    ds.write((char*)&tmp, 2);
  } else if (v < pown(2u, 30)) {
    uint32_t tmp = v.value << 2 | 0b10;
    ds.write((char*)&tmp, 4);
  } else if (v <= std::numeric_limits<uint64_t>::max()) {
    uint64_t tmp = v.value;
    auto s = std::span((char*)&tmp, sizeof(tmp));
    auto nonzero = std::find_if(s.rbegin(), s.rend(), [](const auto& c) {
      if (c != 0)
        return true;
      return false;
    });
    auto trimmed = std::distance(nonzero, s.rend());
    ds.put(((trimmed - 4) << 2) | 0b11);
    ds.write(s.data(), (size_t)trimmed);
  } else {
    // TODO
    check(false, "not implemented");
  }
  return ds;
}

template<typename Stream, UnsignedIntegral T>
datastream<Stream>& operator>>(datastream<Stream>& ds, Varint<T>& v) {
  uint8_t tmp = ds.peek().value();
  switch (tmp & 0b11) {
  case 0b00: {
    uint8_t val = 0;
    ds >> val;
    val >>= 2;
    v = val;
  } break;
  case 0b01: {
    uint16_t val = 0;
    ds >> val;
    val >>= 2;
    v = val;
  } break;
  case 0b10: {
    uint32_t val = 0;
    ds >> val;
    val >>= 2;
    v = val;
  } break;
  case 0b11: {
    uint64_t val = 0;
    auto size = uint8_t(ds.get().value()) >> 2;
    check(size <= 4, "not implemented");
    ds.read((char*)&val, size + 4);
    v = val;
  } break;
  }
  return ds;
}

// Options (bool)
template<typename Stream>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::optional<bool>& v) {
  char val = v.has_value();
  if (val) {
    val += !*v;
  }
  ds << val;
  return ds;
}

template<typename Stream>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::optional<bool>& v) {
  char has_value = 0;
  ds >> has_value;
  if (has_value) {
    v = !(has_value - 1);
  } else {
    v.reset();
  }
  return ds;
}

// Options (except for bool)
template<typename Stream, typename T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::optional<T>& v) {
  char has_value = v.has_value();
  ds << has_value;
  if (has_value)
    ds << *v;
  return ds;
}

template<typename Stream, typename T>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::optional<T>& v) {
  char has_value = 0;
  ds >> has_value;
  if (has_value) {
    T val;
    ds >> val;
    v = val;
  } else {
    v.reset();
  }
  return ds;
}

// Results
template<typename Stream, typename T, typename E>
datastream<Stream>& operator<<(datastream<Stream>& ds, const Result<T, E>& v) {
  char is_unexpected = !v;
  ds << is_unexpected;
  if (v) {
    ds << *v;
  } else {
    ds << v.error();
  }
  return ds;
}

template<typename Stream, typename T, typename E>
datastream<Stream>& operator>>(datastream<Stream>& ds, Result<T, E>& v) {
  char is_unexpected = 0;
  ds >> is_unexpected;
  if (!is_unexpected) {
    T val;
    ds >> val;
    v = val;
  } else {
    E err;
    ds >> err;
    v = err;
  }
  return ds;
}

// Vectors (lists, series, sets)
// TODO: Add other containers
template<typename Stream, typename T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::vector<T>& v) {
  ds << Varuint32(v.size());
  for (const auto& i : v) {
    ds << i;
  }
  return ds;
}

template<typename Stream, typename T>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::vector<T>& v) {
  Varuint32 size;
  ds >> size;
  v.resize(size);
  for (auto& i : v) {
    ds >> i;
  }
  return ds;
}

template<typename Stream, typename T, size_t N>
datastream<Stream>& operator<<(datastream<Stream>& ds, const T (&v)[N]) {
  std::span s(v);
  for (const auto& i : s) {
    ds << i;
  }
  return ds;
}

template<typename Stream, typename T, size_t N>
datastream<Stream>& operator>>(datastream<Stream>& ds, T (&v)[N]) {
  std::span s(v);
  for (auto& i : s) {
    ds >> i;
  }
  return ds;
}

template<typename Stream, ByteSequence T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const T& v) {
  ds.write(v.data(), v.size());
  return ds;
}

template<typename Stream, ByteSequence T>
datastream<Stream>& operator>>(datastream<Stream>& ds, T& v) {
  ds.read(v.data(), v.size());
  return ds;
}

template<typename Stream, size_t N>
datastream<Stream>& operator<<(datastream<Stream>& ds, const BytesN<N>& v) {
  if constexpr (N == std::dynamic_extent) {
    ds << Varuint32(v.size());
  }
  ds.write(v.data(), v.size());
  return ds;
}

template<typename Stream, size_t N>
datastream<Stream>& operator>>(datastream<Stream>& ds, BytesN<N>& v) {
  if constexpr (N == std::dynamic_extent) {
    Varuint32 size = 0;
    ds >> size;
    v.resize(size);
  }
  ds.read(v.data(), v.size());
  return ds;
}

// Strings
template<typename Stream>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::string& v) {
  ds << Varuint32(v.size());
  if (v.size()) {
    ds.write(v.data(), v.size());
  }
  return ds;
}

template<typename Stream>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::string& v) {
  Varuint32 size;
  ds >> size;
  v.resize(size);
  if (size) {
    ds.read(v.data(), v.size());
  }
  return ds;
}

// Tuples
template<typename Stream, typename... Ts>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::tuple<Ts...>& v) {
  std::apply([&](const auto&... val) { ((ds << val), ...); }, v);
  return ds;
}

template<typename Stream, typename... Ts>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::tuple<Ts...>& v) {
  std::apply([&](auto&... val) { ((ds >> val), ...); }, v);
  return ds;
}

// Data Structures
template<typename Stream, Foreachable T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const T& v) {
  for_each_field([&](const auto& val) { ds << val; }, v);
  return ds;
}

template<typename Stream, Foreachable T>
datastream<Stream>& operator>>(datastream<Stream>& ds, T& v) {
  for_each_field([&](auto& val) { ds >> val; }, v);
  return ds;
}

// Enumerations (tagged-unions)
template<typename Stream, typename... Ts>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::variant<Ts...>& v) {
  check(v.index() <= 0xff, "no more than 256 variants are supported");
  uint8_t index = v.index();
  ds << index;
  std::visit([&](auto& val) { ds << val; }, v);
  return ds;
}

namespace detail {
  template<size_t I, typename Stream, typename... Ts>
  void decode(datastream<Stream>& ds, std::variant<Ts...>& v, int i) {
    if constexpr (I < std::variant_size_v<std::variant<Ts...>>) {
      if (i == I) {
        std::variant_alternative_t<I, std::variant<Ts...>> tmp;
        ds >> tmp;
        v.template emplace<I>(std::move(tmp));
      } else {
        decode<I + 1>(ds, v, i);
      }
    } else {
      check(false, "invalid variant index");
    }
  }
} // namespace detail

template<typename Stream, typename... Ts>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::variant<Ts...>& v) {
  uint8_t index = 0;
  ds >> index;
  detail::decode<0>(ds, v, index);
  return ds;
}

// Shared pointer
template<typename Stream, typename T>
datastream<Stream>& operator<<(datastream<Stream>& ds, const std::shared_ptr<T>& v) {
  check(!!v, "shared pointer cannot be null");
  ds << *v;
  return ds;
}

template<typename Stream, typename T>
datastream<Stream>& operator>>(datastream<Stream>& ds, std::shared_ptr<T>& v) {
  v = std::make_shared<T>();
  ds >> *v;
  return ds;
}

} // NOIR_CODEC(scale)
