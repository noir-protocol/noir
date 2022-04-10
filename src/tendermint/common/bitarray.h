// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/common/for_each.h>
#include <noir/common/types/bytes.h>
#include <noir/common/types/varint.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <vector>

namespace tendermint {

/// \brief thread-safe array of bits
class bitarray {
private:
  using word_t = uint64_t;
  static constexpr size_t word_bits = sizeof(word_t) * 8;

public:
  bitarray() = default;
  bitarray(size_t bits): bits(bits), words(_size_words(bits), 0) {}
  bitarray(const bitarray& b) {
    std::scoped_lock g(mtx, b.mtx);
    bits = b.bits;
    words = b.words;
  }
  bitarray& operator=(bitarray&& b) {
    std::scoped_lock g(mtx, b.mtx);
    bits = b.bits;
    words = std::move(b.words);
    return *this;
  }

  size_t size() const {
    return bits;
  }

  auto data() {
    return words.data();
  }

  const auto data() const {
    return words.data();
  }

  size_t size_words() const {
    return _size_words(size());
  }

  bool get(size_t i) const {
    std::scoped_lock g(mtx);
    if (i >= bits)
      return false;
    return (words[i / word_bits] >> (i % word_bits)) & 0b1;
  }

  bool set(size_t i, bool v) {
    std::scoped_lock g(mtx);
    if (i >= bits)
      return false;
    if (v) {
      words[i / word_bits] |= 1 << (i % word_bits);
    } else {
      words[i / word_bits] &= ~(1 << (i % word_bits));
    }
    return true;
  }

  bitarray& update(const bitarray& o) {
    std::scoped_lock g(mtx, o.mtx);
    auto size_ = std::min(bits, o.bits);
    int full = size_ / word_bits;
    int rest = size_ % word_bits;
    for (auto i = 0; i < full - !!rest; ++i) {
      words[i] = o.words[i];
    }
    if (rest) {
      words[full] = (o.words[full] << (word_bits - rest)) >> (word_bits - rest);
    }
    return *this;
  }

  bitarray& sub(const bitarray& o) {
    std::scoped_lock g(mtx, o.mtx);
    auto size_ = std::min(bits, o.bits);
    int full = size_ / word_bits;
    int rest = size_ % word_bits;
    for (auto i = 0; i < full - !!rest; ++i) {
      words[i] &= ~o.words[i];
    }
    if (rest) {
      words[full] &= ~((o.words[full] << (word_bits - rest)) >> (word_bits - rest));
    }
    return *this;
  }

  bitarray& or_op(const bitarray& o) {
    std::scoped_lock g(mtx, o.mtx);
    bits = std::max(bits, o.bits);
    words.resize(size_words());
    std::transform(
      o.words.begin(), o.words.end(), words.begin(), words.begin(), [](auto oi, auto i) { return oi | i; });
    return *this;
  }

  bitarray& not_op() {
    std::scoped_lock g(mtx);
    std::for_each(words.begin(), words.end(), [](auto& i) { i = ~i; });
    size_t rest = bits % word_bits;
    if (rest) {
      words.back() <<= word_bits - rest;
      words.back() >>= word_bits - rest;
    }
    return *this;
  }

  std::string to_string() const {
    std::string ret{};
    for (auto i = 0; i < bits; ++i) {
      ret += get(i) ? "x" : "_";
    }
    return ret;
  }

  noir::bytes to_bytes() const {
    std::scoped_lock g(mtx);
    auto start = (char*)words.data();
    return {start, start + (bits + 7) / 8};
  }

  std::tuple<size_t, bool> pick_random() const {
    std::scoped_lock g(mtx);
    auto true_indices = get_true_indices();
    if (true_indices.empty())
      return {0, false};
    std::vector<size_t> result;
    std::sample(
      true_indices.begin(), true_indices.end(), std::back_inserter(result), 1, std::mt19937{std::random_device{}()});
    return {result[0], true};
  }

  template<typename T>
  inline friend T& operator<<(T& ds, const bitarray& v) {
    ds << noir::varuint32(v.bits);
    ds << std::span((const char*)v.words.data(), (v.bits + 7) / 8);
    return ds;
  }

  template<typename T>
  inline friend T& operator>>(T& ds, bitarray& v) {
    noir::varuint32 bits = 0;
    ds >> bits;
    v = std::move(bitarray(bits));
    ds >> std::span((char*)v.words.data(), (v.bits + 7) / 8);
    return ds;
  }

private:
  size_t bits = 0;
  std::vector<word_t> words;
  mutable std::mutex mtx;

  static constexpr size_t _size_words(size_t bits) {
    return (bits + (word_bits - 1)) / word_bits;
  }

  bitarray copy_bits(size_t bits) const {
    auto size_ = std::min(this->bits, bits);
    int full = size_ / word_bits;
    int rest = size_ % word_bits;
    bitarray o(size_);
    std::copy(words.begin(), words.begin() + full - !!rest, o.words.begin());
    if (rest) {
      o.words[full] = (words[full] << (word_bits - rest)) >> (word_bits - rest);
    }
    return o;
  }

  std::vector<size_t> get_true_indices() const {
    std::vector<size_t> res;
    for (auto i = 0; i < bits; ++i) {
      if (get(i)) {
        res.push_back(i);
      }
    }
    return res;
  }
};

} // namespace tendermint

template<>
struct noir::is_foreachable<tendermint::bitarray> : std::false_type {};
