// This file is part of NOIR.
//
// Copyright (c) 2017-2021 block.one and its contributors.  All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once
#include <noir/common/time.h>
#include <noir/common/types.h>
#include <noir/consensus/bit_array.h>
#include <noir/consensus/block_sync/types.h>
#include <noir/consensus/merkle/proof.h>
#include <noir/p2p/types.h>
#include <tendermint/types/types.pb.h>

#include <google/protobuf/util/time_util.h>
#include <variant>

namespace noir::p2p {

enum signed_msg_type {
  Unknown = 0,
  Prevote = 1,
  Precommit = 2,
  Proposal = 32
};

inline bool is_vote_type_valid(signed_msg_type type) {
  if (type == Prevote || type == Precommit)
    return true;
  return false;
}

struct part_set_header {
  uint32_t total;
  Bytes hash;

  bool operator==(const part_set_header& rhs) const {
    return (total == rhs.total) && (hash == rhs.hash);
  }

  bool is_zero() {
    return (total == 0) && (hash.empty());
  }

  static std::unique_ptr<::tendermint::types::PartSetHeader> to_proto(const part_set_header& p) {
    auto ret = std::make_unique<::tendermint::types::PartSetHeader>();
    ret->set_total(p.total);
    ret->set_hash({p.hash.begin(), p.hash.end()});
    return ret;
  }

  static std::shared_ptr<part_set_header> from_proto(const ::tendermint::types::PartSetHeader& pb) {
    auto ret = std::make_shared<part_set_header>();
    ret->total = pb.total();
    ret->hash = {pb.hash().begin(), pb.hash().end()};
    return ret;
  }
};

struct block_id {
  Bytes hash;
  part_set_header parts;

  bool operator==(const block_id& rhs) const {
    return (hash == rhs.hash) && (parts == rhs.parts);
  }

  bool is_complete() const {
    return (parts.total > 0);
  }

  bool is_zero() {
    return (hash.empty() && parts.is_zero());
  }

  std::string key() {
    // returns a machine-readable string representation of the block_id
    // todo
    return hex::encode(hash) + hex::encode(parts.hash) + std::to_string(parts.total);
  }

  static std::unique_ptr<::tendermint::types::BlockID> to_proto(const block_id& b) {
    auto ret = std::make_unique<::tendermint::types::BlockID>();
    ret->set_hash({b.hash.begin(), b.hash.end()});
    ret->set_allocated_part_set_header(part_set_header::to_proto(b.parts).release());
    return ret;
  }

  static std::shared_ptr<block_id> from_proto(const ::tendermint::types::BlockID& pb) {
    auto ret = std::make_shared<block_id>();
    ret->hash = {pb.hash().begin(), pb.hash().end()};
    ret->parts = *part_set_header::from_proto(pb.part_set_header());
    return ret;
  }
};

///< consensus message starts
struct new_round_step_message {
  int64_t height;
  int32_t round;
  round_step_type step;
  int64_t seconds_since_start_time;
  int32_t last_commit_round;
};

struct new_valid_block_message {
  int64_t height;
  int32_t round;
  part_set_header block_part_set_header;
  std::shared_ptr<consensus::bit_array> block_parts;
  bool is_commit;
};

struct proposal_message {
  signed_msg_type type;
  int64_t height;
  int32_t round;
  int32_t pol_round;
  block_id block_id_;
  tstamp timestamp{0};
  Bytes signature;
};

struct proposal_pol_message {
  int64_t height;
  int32_t proposal_pol_round;
  std::shared_ptr<consensus::bit_array> proposal_pol;
};

struct block_part_message {
  int64_t height;
  int32_t round;
  uint32_t index;
  Bytes bytes_;
  consensus::merkle::proof proof;
};

struct vote_message {
  signed_msg_type type;
  int64_t height;
  int32_t round;
  block_id block_id_;
  tstamp timestamp;
  Bytes validator_address;
  int32_t validator_index;
  Bytes signature;
};

struct has_vote_message {
  int64_t height;
  int32_t round;
  signed_msg_type type;
  int32_t index;
};

struct vote_set_maj23_message {
  int64_t height;
  int32_t round;
  signed_msg_type type;
  block_id block_id_;
};

struct vote_set_bits_message {
  int64_t height;
  int32_t round;
  signed_msg_type type;
  block_id block_id_;
  std::shared_ptr<consensus::bit_array> votes;
};
///< consensus message ends

enum go_away_reason {
  no_reason, ///< no reason to go away
  self, ///< the connection is to itself
  duplicate, ///< the connection is redundant
  wrong_chain, ///< the peer's chain id doesn't match
  unlinkable, ///< the peer sent a block we couldn't use
  validation, ///< the peer sent a block that failed validation
  benign_other, ///< reasons such as a timeout. not fatal but warrant resetting
  fatal_other ///< a catch-all for errors we don't have discriminated
};

constexpr auto reason_str(go_away_reason rsn) {
  switch (rsn) {
  case no_reason:
    return "no reason";
  case self:
    return "self connect";
  case duplicate:
    return "duplicate";
  case wrong_chain:
    return "wrong chain";
  case unlinkable:
    return "unlinkable block received";
  case validation:
    return "invalid block";
  case fatal_other:
    return "some other failure";
  case benign_other:
    return "some other non-fatal condition, possibly unknown block";
  default:
    return "some crazy reason";
  }
}

/// \brief messages that will be delivered to consensus reactor
using cs_reactor_message = std::variant<new_round_step_message,
  new_valid_block_message,
  proposal_message,
  proposal_pol_message,
  block_part_message,
  vote_message,
  has_vote_message,
  vote_set_maj23_message,
  vote_set_bits_message>;

/// \brief messages that will be delivered to block_sync reactor
using bs_reactor_message = std::variant<consensus::block_request,
  consensus::block_response,
  consensus::status_request,
  consensus::status_response,
  consensus::no_block_response>;

/// \brief messages that will be passed from consensus_state to consensus_reactor
using internal_message = std::variant<proposal_message, block_part_message, vote_message>;
struct internal_msg_info {
  internal_message msg;
  std::string peer_id; // TODO: not sure if peer_id field is required
};
using internal_msg_info_ptr = std::shared_ptr<internal_msg_info>;

} // namespace noir::p2p

NOIR_REFLECT(std::chrono::system_clock::duration, );
NOIR_REFLECT(noir::p2p::block_id, hash, parts);
NOIR_REFLECT(noir::p2p::part_set_header, total, hash);
NOIR_REFLECT(noir::p2p::new_round_step_message, height, round, step, seconds_since_start_time, last_commit_round);
NOIR_REFLECT(noir::p2p::new_valid_block_message, height, round, block_part_set_header, block_parts, is_commit);
NOIR_REFLECT(noir::p2p::proposal_message, type, height, round, pol_round, block_id_, timestamp, signature);
NOIR_REFLECT(noir::p2p::proposal_pol_message, height, proposal_pol_round, proposal_pol);
NOIR_REFLECT(noir::p2p::block_part_message, height, round, index, bytes_, proof);
NOIR_REFLECT(noir::p2p::vote_message, type, height, round, block_id_, timestamp, validator_address, validator_index,
  signature);
NOIR_REFLECT(noir::p2p::has_vote_message, height, round, type, index);
NOIR_REFLECT(noir::p2p::vote_set_maj23_message, height, round, type, block_id_);
NOIR_REFLECT(noir::p2p::vote_set_bits_message, height, round, type, block_id_, votes);
