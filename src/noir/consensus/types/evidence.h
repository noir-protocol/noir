// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/consensus/common.h>
#include <noir/consensus/types/light_block.h>
#include <noir/consensus/types/vote.h>
#include <tendermint/abci/types.pb.h>
#include <tendermint/types/evidence.pb.h>

#include <google/protobuf/util/time_util.h>

namespace noir::consensus {

struct evidence {
  virtual std::vector<std::shared_ptr<::tendermint::abci::Evidence>> get_abci() = 0;
  virtual Bytes get_bytes() = 0;
  virtual Bytes get_hash() = 0;
  virtual int64_t get_height() const = 0;
  virtual std::string get_string() = 0;
  virtual tstamp get_timestamp() = 0;
  virtual Result<void> validate_basic() = 0;

  static Result<std::unique_ptr<::tendermint::types::Evidence>> to_proto(const evidence&);
  static Result<std::shared_ptr<evidence>> from_proto(const ::tendermint::types::Evidence&);
};

struct duplicate_vote_evidence : public evidence {
  std::shared_ptr<vote> vote_a{};
  std::shared_ptr<vote> vote_b{};
  int64_t total_voting_power{};
  int64_t validator_power{};
  tstamp timestamp{};

  static Result<std::shared_ptr<duplicate_vote_evidence>> new_duplicate_vote_evidence(
    const std::shared_ptr<vote>& vote1,
    const std::shared_ptr<vote>& vote2,
    tstamp block_time,
    const std::shared_ptr<validator_set>& val_set) {
    if (!vote1 || !vote2)
      return Error::format("missing vote");
    if (!val_set)
      return Error::format("missing validator_set");
    auto val = val_set->get_by_address(vote1->validator_address);
    if (!val.has_value())
      return Error::format("validator is not in validator_set");
    std::shared_ptr<vote> vote_a_, vote_b_;
    if (vote1->block_id_.key() < vote2->block_id_.key()) {
      vote_a_ = vote1;
      vote_b_ = vote2;
    } else {
      vote_a_ = vote2;
      vote_b_ = vote1;
    }
    auto ret = std::make_shared<duplicate_vote_evidence>();
    ret->vote_a = vote_a_;
    ret->vote_b = vote_b_;
    ret->total_voting_power = val_set->total_voting_power;
    ret->validator_power = val->voting_power;
    ret->timestamp = block_time;
    return ret;
  }

  std::vector<std::shared_ptr<::tendermint::abci::Evidence>> get_abci() override {
    auto ev = std::make_shared<::tendermint::abci::Evidence>();
    ev->set_type(::tendermint::abci::DUPLICATE_VOTE);
    auto val = ev->mutable_validator();
    *val->mutable_address() = std::string(vote_a->validator_address.begin(), vote_a->validator_address.end());
    val->set_power(validator_power);
    ev->set_height(vote_a->height);
    *ev->mutable_time() = ::google::protobuf::util::TimeUtil::MicrosecondsToTimestamp(timestamp);
    ev->set_total_voting_power(total_voting_power);
    return {ev};
  }

  Bytes get_bytes() override {
    auto pbe = to_proto(*this);
    Bytes ret(pbe->ByteSizeLong());
    pbe->SerializeToArray(ret.data(), pbe->ByteSizeLong());
    return ret;
  }

  Bytes get_hash() override {
    return crypto::Sha256()(get_bytes());
  }

  int64_t get_height() const override {
    return vote_a->height;
  }

  std::string get_string() override {
    return fmt::format(
      "duplicate_vote_evidence(vote_a, vote_b, total_voting_power={}, validator_power={}, timestamp={})",
      total_voting_power, validator_power, timestamp);
  }

  tstamp get_timestamp() override {
    return timestamp;
  }

  Result<void> validate_basic() override {
    if (!vote_a || !vote_b)
      return Error::format("one or both of votes are empty");
    // if (vote_a->validate_basic)
    // if (vote_b->validate_basic)
    if (vote_a->block_id_.key() >= vote_b->block_id_.key())
      return Error::format("duplicate votes in invalid order");
    return success();
  }

  Result<void> validate_abci(
    std::shared_ptr<validator> val, std::shared_ptr<validator_set> val_set, tstamp evidence_time) {
    if (timestamp != evidence_time)
      return Error::format("evidence has a different time to the block it is associated with");
    if (val->voting_power != validator_power)
      return Error::format("validator power from evidence and our validator set does not match");
    if (val_set->get_total_voting_power() != total_voting_power)
      return Error::format("total voting power from the evidence and our validator set does not match");
    return success();
  }

  void generate_abci(std::shared_ptr<validator> val, std::shared_ptr<validator_set> val_set, tstamp evidence_time) {
    validator_power = val->voting_power;
    total_voting_power = val_set->total_voting_power;
    timestamp = evidence_time;
  }

  static std::unique_ptr<::tendermint::types::DuplicateVoteEvidence> to_proto(const duplicate_vote_evidence& ev) {
    auto ret = std::make_unique<::tendermint::types::DuplicateVoteEvidence>();
    if (ev.vote_b)
      ret->set_allocated_vote_b(vote::to_proto(*ev.vote_b).release());
    if (ev.vote_a)
      ret->set_allocated_vote_a(vote::to_proto(*ev.vote_a).release());
    ret->set_total_voting_power(ev.total_voting_power);
    ret->set_validator_power(ev.validator_power);
    *ret->mutable_timestamp() = ::google::protobuf::util::TimeUtil::MicrosecondsToTimestamp(ev.timestamp);
    return ret;
  }

  static Result<std::shared_ptr<duplicate_vote_evidence>> from_proto(::tendermint::types::DuplicateVoteEvidence& pb) {
    if (!pb.IsInitialized())
      return Error::format("from_proto failed: duplicate vote evidence is not initialized");
    auto ret = std::make_shared<duplicate_vote_evidence>();
    ret->vote_a = vote::from_proto(const_cast<::tendermint::types::Vote&>(pb.vote_a()));
    ret->vote_b = vote::from_proto(const_cast<::tendermint::types::Vote&>(pb.vote_b()));
    ret->total_voting_power = pb.total_voting_power();
    ret->validator_power = pb.validator_power();
    ret->timestamp = ::google::protobuf::util::TimeUtil::TimestampToMicroseconds(pb.timestamp());
    return ret;
  }

  template<typename T>
  friend T& operator<<(T& ds, const duplicate_vote_evidence& v) {
    auto pb = duplicate_vote_evidence::to_proto(v);
    Bytes bz(pb->ByteSizeLong());
    pb->SerializeToArray(bz.data(), pb->ByteSizeLong());
    ds << bz.size();
    ds << bz;
    return ds;
  }
  template<typename T>
  friend T& operator>>(T& ds, duplicate_vote_evidence& v) {
    size_t len;
    ds >> len;
    Bytes bz(len);
    ds >> bz;
    ::tendermint::types::DuplicateVoteEvidence pb;
    pb.ParseFromArray(bz.data(), bz.size());
    auto decoded = duplicate_vote_evidence::from_proto(pb);
    // check(!!decoded, decoded.error().message());
    v = *decoded.value();
    return ds;
  }
};

struct light_client_attack_evidence : public evidence {
  std::shared_ptr<light_block> conflicting_block;
  int64_t common_height;

  // ABCI specific info
  std::vector<std::shared_ptr<validator>> byzantine_validators;
  int64_t total_voting_power;
  tstamp timestamp;

  std::vector<std::shared_ptr<::tendermint::abci::Evidence>> get_abci() override;

  Bytes get_bytes() override {
    auto pbe = to_proto(*this);
    if (!pbe)
      check(false, fmt::format("converting light client attack evidence to proto: {}", pbe.error()));
    Bytes ret(pbe.value()->ByteSizeLong());
    pbe.value()->SerializeToArray(ret.data(), pbe.value()->ByteSizeLong());
    return ret;
  }

  Bytes get_hash() override;

  int64_t get_height() const override {
    return common_height;
  }

  std::string get_string() override {
    return fmt::format("light_client_attack_evidence #{}", hex::encode(get_hash()));
  }

  tstamp get_timestamp() override {
    return timestamp;
  }

  Result<void> validate_basic() override {
    if (!conflicting_block)
      return Error::format("conflicting block is null");
    if (!conflicting_block->s_header)
      return Error::format("conflicting block is missing header");
    if (total_voting_power <= 0)
      return Error::format("negative or zero total voting power");
    if (common_height <= 0)
      return Error::format("negative or zero common height");
    if (common_height > conflicting_block->s_header->header->height)
      return Error::format("common height is ahead of conflicting block height");
    auto ok = conflicting_block->validate_basic(conflicting_block->s_header->header->chain_id);
    if (!ok)
      return Error::format("invalid conflicting light block: {}", ok.error().message());
    return success();
  }

  Result<void> validate_abci(
    std::shared_ptr<validator_set> common_vals, std::shared_ptr<signed_header> trusted_header, tstamp evidence_time) {
    auto ev_total = total_voting_power;
    auto vals_total = common_vals->total_voting_power;
    if (ev_total != vals_total)
      return Error::format("total voting power from evidence and our validator set does not match");
    if (timestamp != evidence_time)
      return Error::format("evidence has a different time to the block it is associated with");

    auto validators = get_byzantine_validators(common_vals, trusted_header);
    if (validators.empty() && !byzantine_validators.empty())
      return Error::format("expected zero validators from an amnesia light client attack but got some");

    if (validators.size() != byzantine_validators.size())
      return Error::format("unexpected number of byzantine validators from evidence");

    int idx{0};
    for (auto& val : validators) {
      if (byzantine_validators[idx]->address != val->address)
        return Error::format("evidence contained an unexpected byzantine validator address");
      if (byzantine_validators[idx]->voting_power != val->voting_power)
        return Error::format("evidence contained an unexpected byzantine validator power");
      idx++;
    }
    return success();
  }

  void generate_abci(
    std::shared_ptr<validator_set> common_vals, std::shared_ptr<signed_header> trusted_header, tstamp evidence_time) {
    timestamp = evidence_time;
    total_voting_power = common_vals->total_voting_power;
    byzantine_validators = get_byzantine_validators(common_vals, trusted_header);
  }

  static Result<std::unique_ptr<::tendermint::types::LightClientAttackEvidence>> to_proto(
    const light_client_attack_evidence& ev) {
    auto cb = light_block::to_proto(*ev.conflicting_block);
    auto ret = std::make_unique<::tendermint::types::LightClientAttackEvidence>();
    ret->set_allocated_conflicting_block(cb.release());
    ret->set_common_height(ev.common_height);
    auto byz_vals = ret->mutable_byzantine_validators();
    for (auto& val : ev.byzantine_validators) {
      auto pb = validator::to_proto(*val);
      byz_vals->AddAllocated(pb.release());
    }
    ret->set_total_voting_power(ev.total_voting_power);
    *ret->mutable_timestamp() = ::google::protobuf::util::TimeUtil::MicrosecondsToTimestamp(ev.timestamp);
    return ret;
  }

  static Result<std::shared_ptr<light_client_attack_evidence>> from_proto(
    ::tendermint::types::LightClientAttackEvidence& pb) {
    if (!pb.IsInitialized())
      return Error::format("from_proto failed: light client attack evidence is not initialized");
    auto ret = std::make_shared<light_client_attack_evidence>();
    auto conflicting_block = light_block::from_proto(pb.conflicting_block());
    if (!conflicting_block)
      return conflicting_block.error();
    for (auto& v : pb.byzantine_validators()) {
      if (auto ok = validator::from_proto(v); !ok)
        return ok.error();
      else
        ret->byzantine_validators.push_back(ok.value());
    }
    ret->conflicting_block = conflicting_block.value();
    ret->common_height = pb.common_height();
    ret->total_voting_power = pb.total_voting_power();
    ret->timestamp = ::google::protobuf::util::TimeUtil::TimestampToMicroseconds(pb.timestamp());
    if (auto ok = ret->validate_basic(); !ok)
      return ok.error();
    return ret;
  }

  std::vector<std::shared_ptr<validator>> get_byzantine_validators(
    const std::shared_ptr<validator_set>& common_vals, const std::shared_ptr<signed_header>& trusted);

  bool conflicting_header_is_invalid(const std::shared_ptr<block_header>& trusted_header) const;

  template<typename T>
  friend T& operator<<(T& ds, const light_client_attack_evidence& v) {
    auto pb = light_client_attack_evidence::to_proto(v);
    Bytes bz(pb.value()->ByteSizeLong());
    pb.value()->SerializeToArray(bz.data(), pb.value()->ByteSizeLong());
    ds << bz.size();
    ds << bz;
  }
  template<typename T>
  friend T& operator>>(T& ds, light_client_attack_evidence& v) {
    size_t len;
    ds >> len;
    Bytes bz(len);
    ds >> bz;
    ::tendermint::types::LightClientAttackEvidence pb;
    pb.ParseFromArray(bz.data(), bz.size());
    auto decoded = light_client_attack_evidence::from_proto(pb);
    // check(!!decoded, decoded.error().message());
    v = *decoded.value();
    return ds;
  }
};

struct evidence_list {
  std::vector<std::shared_ptr<evidence>> list;

  Bytes hash() {
    std::vector<Bytes> bytes_list;
    for (auto& e : list)
      bytes_list.push_back(e->get_bytes()); // TODO : use hash when golang implementation uses hash in the future
    return consensus::merkle::hash_from_bytes_list(bytes_list);
  }

  std::string to_string() {
    std::string ret;
    for (auto& e : list)
      ret += fmt::format("{}\t\t", e->get_string());
    return ret;
  }

  bool has(const std::shared_ptr<evidence>& ev) {
    for (auto& e : list) {
      if (e->get_hash() == ev->get_hash())
        return true;
    }
    return false;
  }

  static Result<std::unique_ptr<::tendermint::types::EvidenceList>> to_proto(const evidence_list& evs) {
    auto ret = std::make_unique<::tendermint::types::EvidenceList>();
    auto evi = ret->mutable_evidence();
    for (auto& ev : evs.list) {
      auto pb = evidence::to_proto(*ev);
      if (!pb)
        return pb.error();
      evi->AddAllocated(pb.value().release());
    }
    return ret;
  }

  static Result<std::shared_ptr<evidence_list>> from_proto(const ::tendermint::types::EvidenceList& pb) {
    auto ret = std::make_shared<evidence_list>();
    for (auto& i : pb.evidence()) {
      auto ev = evidence::from_proto(i);
      if (!ev)
        return ev.error();
      ret->list.push_back(ev.value());
    }
    return ret;
  }

  template<typename T>
  friend T& operator<<(T& ds, const evidence_list& v) {
    ds << v.list.size();
    for (auto& e : v.list) {
      auto pb = evidence::to_proto(*e);
      // check(!!pb, pb.error().message());
      Bytes bz(pb.value()->ByteSizeLong());
      pb.value()->SerializeToArray(bz.data(), pb.value()->ByteSizeLong());
      ds << bz.size();
      ds << bz;
    }
    return ds;
  }

  template<typename T>
  friend T& operator>>(T& ds, evidence_list& v) {
    size_t size{0};
    ds >> size;
    for (auto i = 0; i < size; i++) {
      size_t len;
      ds >> len;
      Bytes bz(len);
      ds >> bz;
      ::tendermint::types::Evidence pb;
      pb.ParseFromArray(bz.data(), bz.size());
      auto decoded = evidence::from_proto(pb);
      // check(!!decoded, decoded.error().message());
      v.list.push_back(decoded.value());
    }
    return ds;
  }
};

} // namespace noir::consensus

template<>
struct noir::IsForeachable<noir::consensus::duplicate_vote_evidence> : std::false_type {};
template<>
struct noir::IsForeachable<noir::consensus::light_client_attack_evidence> : std::false_type {};
template<>
struct noir::IsForeachable<noir::consensus::evidence_list> : std::false_type {};
