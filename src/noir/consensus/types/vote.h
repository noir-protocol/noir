// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/consensus/types/block.h>
#include <noir/consensus/types/validator.h>
#include <noir/p2p/protocol.h>
#include <noir/p2p/types.h>

#include <fmt/core.h>

namespace noir::consensus {

extern const Error ErrGotVoteFromUnwantedRound;
extern const Error ErrVoteInvalidValidatorIndex;
extern const Error ErrVoteInvalidValidatorAddress;
extern const Error ErrVoteNonDeterministicSignature;
extern const Error ErrVoteConflictingVotes;
class ErrVoteConflictingVotesWithData : public Error {
public:
  ErrVoteConflictingVotesWithData(const std::shared_ptr<vote>& a, const std::shared_ptr<vote>& b);
  std::shared_ptr<vote> vote_a;
  std::shared_ptr<vote> vote_b;
};

using P2PID = std::string;

/**
 * represents a prevote, precommit, or commit vote from validators for consensus
 */
struct vote : p2p::vote_message {

  commit_sig to_commit_sig() {
    block_id_flag flag;
    if (block_id_.is_complete())
      flag = FlagCommit;
    else if (block_id_.is_zero())
      flag = FlagNil;
    else
      throw std::runtime_error(fmt::format("Invalid vote - expected block_id to be either empty or complete"));
    return commit_sig{flag, validator_address, timestamp, signature};
  }

  static std::unique_ptr<::tendermint::types::Vote> to_proto(const vote& v) {
    auto ret = std::make_unique<::tendermint::types::Vote>();
    ret->set_type((::tendermint::types::SignedMsgType)v.type);
    ret->set_height(v.height);
    ret->set_round(v.round);
    ret->set_allocated_block_id(p2p::block_id::to_proto(v.block_id_).release());
    auto ts = ret->mutable_timestamp();
    *ts = ::google::protobuf::util::TimeUtil::MicrosecondsToTimestamp(v.timestamp); // TODO
    ret->set_validator_address({v.validator_address.begin(), v.validator_address.end()});
    ret->set_validator_index(v.validator_index);
    ret->set_signature({v.signature.begin(), v.signature.end()});
    return ret;
  }

  static std::shared_ptr<vote> from_proto(const ::tendermint::types::Vote& pb) {
    auto ret = std::make_shared<vote>();
    ret->type = (p2p::signed_msg_type)pb.type();
    ret->height = pb.height();
    ret->round = pb.round();
    ret->block_id_ = *p2p::block_id::from_proto(const_cast<::tendermint::types::BlockID&>(pb.block_id()));
    ret->timestamp = ::google::protobuf::util::TimeUtil::TimestampToMicroseconds(pb.timestamp()); // TODO
    ret->validator_address = {pb.validator_address().begin(), pb.validator_address().end()};
    ret->validator_index = pb.validator_index();
    ret->signature = {pb.signature().begin(), pb.signature().end()};
    return ret;
  }

  static Bytes vote_sign_bytes(const std::string& chain_id, const ::tendermint::types::Vote& v);
};

struct block_votes {
  bool peer_maj23{};
  std::shared_ptr<bit_array> bit_array_;
  std::vector<std::shared_ptr<vote>> votes;
  int64_t sum{};

  static std::shared_ptr<block_votes> new_block_votes(bool peer_maj23_, int num_validators) {
    auto ret = std::make_shared<block_votes>();
    ret->peer_maj23 = peer_maj23_;
    ret->bit_array_ = bit_array::new_bit_array(num_validators);
    ret->votes.resize(num_validators);
    // std::fill(ret->votes.begin(), ret->votes.end(), nullptr);
    return ret;
  }

  void add_verified_vote(const std::shared_ptr<vote>& vote_, int64_t voting_power) {
    auto val_index = vote_->validator_index;
    if (votes.size() > val_index) {
      if (!votes[val_index]) {
        bit_array_->set_index(val_index, true);
        votes[val_index] = vote_;
        sum += voting_power;
      }
    }
  }

  std::shared_ptr<vote> get_by_index(int32_t index) {
    if (!votes.empty() && votes.size() > index)
      return votes[index];
    return {};
  }
};

/**
 * VoteSet helps collect signatures from validators at each height+round for a
 * predefined vote type.
 *
 * We need VoteSet to be able to keep track of conflicting votes when validators
 * double-sign.  Yet, we can't keep track of *all* the votes seen, as that could
 * be a DoS attack vector.
 *
 * There are two storage areas for votes.
 * 1. voteSet.votes
 * 2. voteSet.votesByBlock
 *
 * `.votes` is the "canonical" list of votes.  It always has at least one vote,
 * if a vote from a validator had been seen at all.  Usually it keeps track of
 * the first vote seen, but when a 2/3 majority is found, votes for that get
 * priority and are copied over from `.votesByBlock`.
 *
 * `.votesByBlock` keeps track of a list of votes for a particular block.  There
 * are two ways a &blockVotes{} gets created in `.votesByBlock`.
 * 1. the first vote seen by a validator was for the particular block.
 * 2. a peer claims to have seen 2/3 majority for the particular block.
 *
 * Since the first vote from a validator will always get added in `.votesByBlock`
 * , all votes in `.votes` will have a corresponding entry in `.votesByBlock`.
 *
 * When a &blockVotes{} in `.votesByBlock` reaches a 2/3 majority quorum, its
 * votes are copied into `.votes`.
 *
 * All this is memory bounded because conflicting votes only get added if a peer
 * told us to track that block, each peer only gets to tell us 1 such block, and,
 * there's only a limited number of peers.
 *
 * NOTE: Assumes that the sum total of voting power does not exceed MaxUInt64.
 */
struct vote_set {
  std::string chain_id;
  int64_t height;
  int32_t round;
  p2p::signed_msg_type signed_msg_type_;
  std::shared_ptr<validator_set> val_set{};

  std::mutex mtx;
  std::shared_ptr<bit_array> votes_bit_array{};
  std::vector<std::shared_ptr<vote>> votes;
  int64_t sum;
  std::optional<p2p::block_id> maj23;
  std::map<std::string, std::shared_ptr<block_votes>> votes_by_block;
  std::map<P2PID, p2p::block_id> peer_maj23s;

  static std::shared_ptr<vote_set> new_vote_set(const std::string& chain_id_,
    int64_t height_,
    int32_t round_,
    p2p::signed_msg_type signed_msg_type,
    const std::shared_ptr<validator_set>& val_set_);

  std::shared_ptr<bit_array> get_bit_array();

  virtual int get_size() const {
    return val_set->size();
  }

  std::pair<bool, Error> add_vote(const std::shared_ptr<vote>& vote_);

  std::shared_ptr<vote> get_vote(int32_t val_index, const std::string& block_key) {
    if (votes.size() > 0 && votes.size() > val_index && votes[val_index]) {
      auto& existing = votes[val_index];
      if (existing->block_id_.key() == block_key)
        return existing;
    }
    if (votes_by_block.contains(block_key)) {
      auto existing = votes_by_block[block_key]->get_by_index(val_index);
      if (!existing)
        return existing;
    }
    return {};
  }

  std::shared_ptr<bit_array> bit_array_by_block_id(p2p::block_id block_id_) {
    std::scoped_lock g(mtx);
    auto it = votes_by_block.find(block_id_.key());
    if (it != votes_by_block.end())
      return it->second->bit_array_->copy();
    return {};
  }

  std::optional<std::string> set_peer_maj23(std::string peer_id, p2p::block_id block_id_) {
    std::scoped_lock g(mtx);

    auto block_key = block_id_.key();

    // Make sure peer has not sent us something yet
    if (auto it = peer_maj23s.find(peer_id); it != peer_maj23s.end()) {
      if (it->second == block_id_)
        return {}; // nothing to do
      return "setPeerMaj23: Received conflicting blockID";
    }
    peer_maj23s[peer_id] = block_id_;

    // Create votes_by_block
    auto it = votes_by_block.find(block_key);
    if (it != votes_by_block.end()) {
      if (it->second->peer_maj23)
        return {}; // nothing to do
      it->second->peer_maj23 = true;
    } else {
      auto new_votes_by_block = block_votes::new_block_votes(true, val_set->size());
      votes_by_block[block_key] = new_votes_by_block;
    }
    return {};
  }

  bool has_two_thirds_majority() {
    std::scoped_lock g(mtx);
    return maj23.has_value();
  }

  bool has_two_thirds_any() {
    std::scoped_lock g(mtx);
    return sum > val_set->total_voting_power * 2 / 3;
  }

  bool has_all() {
    std::scoped_lock g(mtx);
    return sum == val_set->total_voting_power;
  }

  /**
   * if there is a 2/3+ majority for block_id, return block_id
   */
  std::optional<p2p::block_id> two_thirds_majority() {
    std::scoped_lock g(mtx);
    // if (maj23.has_value())
    //  return maj23;
    // return {};
    return maj23;
  }

  /**
   * constructs a commit from the vote_set. It only include precommits for the block, which has 2/3+ majority and nil
   */
  std::shared_ptr<commit> make_commit() {
    std::scoped_lock g(mtx);
    if (signed_msg_type_ != p2p::Precommit)
      throw std::runtime_error("cannot make_commit() unless signed_msg_type_ is Precommit");
    // Make sure we have a 2/3 majority
    if (!maj23.has_value())
      throw std::runtime_error("cannot make_comit() unless a block has 2/3+");
    // For every validator, get the precommit
    std::vector<commit_sig> commit_sigs;
    commit_sigs.resize(votes.size());
    for (auto i = 0; auto& vote : votes) {
      auto commit_sig_ = (vote) ? vote->to_commit_sig() : commit_sig::new_commit_sig_absent();
      // If block_id exists but does not match, exclude sig
      if (commit_sig_.for_block() && (vote->block_id_ != maj23)) {
        commit_sig_ = commit_sig::new_commit_sig_absent();
      }
      commit_sigs[i++] = commit_sig_;
    }
    return commit::new_commit(height, round, maj23.value(), commit_sigs);
  }
};

struct nil_vote_set : vote_set {
  nil_vote_set() {
    height = 0;
    round = -1;
    signed_msg_type_ = p2p::signed_msg_type::Unknown;
  }
  int get_size() const override {
    return 0;
  }
};

struct vote_set_reader {
  int64_t height;
  int32_t round;
  std::shared_ptr<bit_array> bit_array_;
  bool is_commit;
  p2p::signed_msg_type type;
  int size;
  std::vector<std::shared_ptr<vote>> votes;

  vote_set_reader() = delete;
  vote_set_reader(const vote_set_reader&) = delete;
  vote_set_reader(vote_set_reader&&) = default;
  vote_set_reader& operator=(const vote_set_reader&) = delete;

  explicit vote_set_reader(const commit& commit_) {
    height = commit_.height;
    round = commit_.round;
    bit_array_ = commit_.bit_array_;
    if (!bit_array_) {
      bit_array_ = bit_array::new_bit_array(commit_.signatures.size());
      for (auto i = 0; i < commit_.signatures.size(); i++)
        bit_array_->set_index(i, commit_.signatures[i].flag != FlagAbsent);
    }
    is_commit = commit_.signatures.size() != 0;
    type = p2p::Precommit;
    size = commit_.signatures.size();
    int i{0};
    for (auto sig : commit_.signatures) {
      votes.push_back(std::make_shared<vote>(vote{p2p::Precommit, commit_.height, commit_.round,
        sig.get_block_id(commit_.my_block_id), sig.timestamp, sig.validator_address, i++, sig.signature}));
    }
  }
  explicit vote_set_reader(const vote_set& vote_set_) {
    height = vote_set_.height;
    round = vote_set_.round;
    bit_array_ = vote_set_.votes_bit_array;
    if (vote_set_.signed_msg_type_ != p2p::Precommit)
      is_commit = false;
    else
      is_commit = vote_set_.maj23.has_value();
    type = vote_set_.signed_msg_type_;
    size = vote_set_.val_set ? vote_set_.val_set->size() : 0;
    votes = vote_set_.votes;
  }

  std::shared_ptr<vote> get_by_index(int32_t val_index) {
    if (!votes.empty() && votes.size() > val_index)
      return votes[val_index];
    return nullptr;
  }
};

} // namespace noir::consensus

NOIR_REFLECT_DERIVED(noir::consensus::vote, noir::p2p::vote_message);
