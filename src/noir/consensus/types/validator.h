// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/consensus/crypto.h>
#include <noir/core/result.h>
#include <noir/p2p/protocol.h>
#include <noir/p2p/types.h>
#include <tendermint/types/types.pb.h>

namespace noir::consensus {

// MaxTotalVotingPower - the maximum allowed total voting power.
// It needs to be sufficiently small to, in all cases:
// 1. prevent clipping in incrementProposerPriority()
// 2. let (diff+diffMax-1) not overflow in IncrementProposerPriority()
// (Proof of 1 is tricky, left to the reader).
// It could be higher, but this is sufficiently large for our purposes,
// and leaves room for defensive purposes.
constexpr int64_t max_total_voting_power{std::numeric_limits<int64_t>::max() / 8};

// PriorityWindowSizeFactor - is a constant that when multiplied with the
// total voting power gives the maximum allowed distance between validator
// priorities.
constexpr int64_t priority_window_size_factor{2};

struct validator {
  Bytes address;
  pub_key pub_key_;
  int64_t voting_power;
  int64_t proposer_priority;

  validator& compare_proposer_priority(validator& other) {
    if (this == &other)
      return *this;
    if (proposer_priority > other.proposer_priority)
      return *this;
    if (proposer_priority < other.proposer_priority)
      return other;
    if (address.empty() || other.address.empty())
      throw std::runtime_error("unable to compare validators as address is empty");
    auto cmp = std::memcmp(address.data(), other.address.data(), std::min(address.size(), other.address.size()));
    if (cmp < 0) {
      return *this;
    }
    return other;
  }

  Bytes get_bytes();

  static validator new_validator(pub_key key, int64_t voting_power) {
    return validator{key.address(), key, voting_power, 0};
  }

  static std::unique_ptr<::tendermint::types::Validator> to_proto(const validator& v) {
    auto ret = std::make_unique<::tendermint::types::Validator>();
    ret->set_address({v.address.begin(), v.address.end()});
    ret->set_allocated_pub_key(pub_key::to_proto(v.pub_key_).value().release()); // TODO: handle error case
    ret->set_voting_power(v.voting_power);
    ret->set_proposer_priority(v.proposer_priority);
    return ret;
  }

  static Result<std::shared_ptr<validator>> from_proto(const ::tendermint::types::Validator& pb) {
    auto ret = std::make_shared<validator>();
    if (auto ok = pub_key::from_proto(pb.pub_key()); ok)
      ret->pub_key_ = *ok.value();
    else
      return ok.error();
    ret->address = {pb.address().begin(), pb.address().end()};
    ret->voting_power = pb.voting_power();
    ret->proposer_priority = pb.proposer_priority();
    return ret;
  }
};

struct validator_set : public std::enable_shared_from_this<validator_set> {
  std::vector<validator> validators;
  std::optional<validator> proposer;
  int64_t total_voting_power = 0;
  // private:
  // validator_set() = default;

public:
  [[nodiscard]] static std::shared_ptr<validator_set> new_validator_set(const std::vector<validator>& validator_list) {
    auto ret = std::shared_ptr<validator_set>(new validator_set());
    ret->update_with_change_set(validator_list, false);
    if (!validator_list.empty())
      ret->increment_proposer_priority(1);
    return ret;
  }

  int size() const {
    return validators.size();
  }

  std::shared_ptr<validator_set> copy() {
    auto copy_ = std::make_shared<validator_set>();
    *copy_ = *this;
    return copy_;
  }

  Bytes get_hash();

  bool has_address(const Bytes& address) {
    for (const auto& val : validators) {
      if (val.address == address)
        return true;
    }
    return false;
  }

  std::optional<validator> get_by_address(const Bytes& address) {
    for (auto val : validators) {
      if (val.address == address)
        return val;
    }
    return {};
  }

  int32_t get_index_by_address(const Bytes& address) {
    for (auto idx = 0; idx < validators.size(); idx++) {
      if (validators[idx].address == address)
        return idx;
    }
    return -1;
  }

  std::optional<validator> get_by_index(int32_t index) {
    if (index < 0 || index >= validators.size())
      return {};
    return validators.at(index);
  }

  int64_t get_total_voting_power() {
    if (total_voting_power == 0)
      update_total_voting_power();
    return total_voting_power;
  }

  void update_total_voting_power() {
    int64_t sum{};
    for (const auto& val : validators) {
      sum += val.voting_power; // todo - check safe add
      if (sum > max_total_voting_power)
        throw std::runtime_error("total_voting_power exceeded max allowed");
    }
    total_voting_power = sum;
  }

  std::optional<validator> get_proposer() {
    if (validators.empty())
      return {};
    if (!proposer.has_value())
      proposer = find_proposer();
    return proposer;
  }

  std::optional<validator> find_proposer() {
    if (validators.empty())
      return {};
    auto val_with_most_priority = validators.at(0);
    for (auto& val : validators) {
      val_with_most_priority = val.compare_proposer_priority(val_with_most_priority);
    }
    return val_with_most_priority;
  }

  /** \brief Merges the vals' validator list with the updates list.
   * When two elements with same address are seen, the one from updates is selected.
   * Expects updates to be a list of updates sorted by address with no duplicates or errors,
   * must have been validated with verifyUpdates() and priorities computed with computeNewPriorities().
   */
  void apply_updates(std::vector<validator>& updates) {
    std::vector<validator> existing(validators);
    sort(existing.begin(), existing.end(), [](validator a, validator b) { return a.address < b.address; });

    std::vector<validator> merged;
    merged.resize(existing.size() + updates.size());
    auto i = 0;
    while (!existing.empty() && !updates.empty()) {
      if (existing[0].address < updates[0].address) {
        merged[i] = existing[0];
        existing.erase(existing.begin());
      } else {
        // apply add or update
        merged[i] = updates[0];
        if (existing[0].address == updates[0].address) {
          // validator is present in both, advance existing
          existing.erase(existing.begin());
        }
        updates.erase(updates.begin());
      }
      i++;
    }

    // add the elements which are left
    for (auto& j : existing) {
      merged[i] = j;
      i++;
    }
    // Or, add updates which are left
    for (auto& update : updates) {
      merged[i] = update;
      i++;
    }

    validators.clear();
    for (auto j = 0; j < i; j++)
      validators.push_back(merged[j]);
  }

  /** \brief Removes the validators specified in 'deletes' from validator set 'vals'.
   * Should not fail as verification has been done before.
   * Expects vals to be sorted by address (done by applyUpdates).
   */
  void apply_removals(std::vector<validator>& deletes) {
    std::vector<validator> existing(validators);

    std::vector<validator> merged;
    merged.resize(existing.size() - deletes.size());
    auto i = 0;
    // Loop over deletes until we removed all of them.
    while (!deletes.empty()) {
      if (existing[0].address == deletes[0].address) {
        deletes.erase(deletes.begin());
      } else {
        // Leave it in the resulting slice.
        merged[i] = existing[0];
        i++;
      }
      existing.erase(existing.begin());
    }

    // add the elements which are left
    for (auto& j : existing) {
      merged[i] = j;
      i++;
    }

    validators.clear();
    for (auto j = 0; j < i; j++)
      validators.push_back(merged[j]);
  }

  /** \brief attempts to update the validator set with 'changes'.
   * It performs the following steps:
   * - validates the changes making sure there are no duplicates and splits them in updates and deletes
   * - verifies that applying the changes will not result in errors
   * - computes the total voting power BEFORE removals to ensure that in the next steps the priorities
   *   across old and newly added validators are fair
   * - computes the priorities of new validators against the final set
   * - applies the updates against the validator set
   * - applies the removals against the validator set
   * - performs scaling and centering of priority values
   * If an error is detected during verification steps, it is returned and the validator set
   * is not changed.
   */
  Result<void> update_with_change_set(const std::vector<validator>& changes, bool allow_deletes) {
    if (changes.empty())
      return success();

    // Check for duplicates within changes, split in 'updates' and 'deletes' lists (sorted).
    std::vector<validator> changesCopy(changes);
    sort(changesCopy.begin(), changesCopy.end(), [](validator a, validator b) { return a.address < b.address; });
    std::vector<validator> updates, deletes;
    Bytes prevAddr;
    for (auto val_update : changesCopy) {
      if (val_update.address == prevAddr)
        return Error::format("duplicate entry {} in changes", to_hex(val_update.address));
      if (val_update.voting_power < 0) {
        return Error::format("voting power can't be negative: {}", val_update.voting_power);
      } else if (val_update.voting_power > max_total_voting_power) {
        return Error::format(
          "to prevent clipping/overflow, voting power can't be higher than max allowed: {}", val_update.voting_power);
      } else if (val_update.voting_power == 0) {
        deletes.push_back(val_update);
      } else {
        updates.push_back(val_update);
      }
      prevAddr = val_update.address;
    }

    if (!allow_deletes && !deletes.empty())
      return Error::format("cannot process validators with voting power 0");

    // Check that the resulting set will not be empty.
    auto num_new_validators = 0;
    for (const auto& val_update : updates) {
      if (!has_address(val_update.address))
        num_new_validators++;
    }
    if (num_new_validators == 0 && validators.size() == deletes.size())
      return Error::format("applying the validator changes would result in empty set");

    // Verify that applying the 'deletes' against 'vals' will not result in error.
    // Get the voting power that is going to be removed.
    int64_t removed_voting_power = 0;
    for (auto& val_update : deletes) {
      auto address = val_update.address;
      auto val = get_by_address(address);
      if (!val.has_value())
        return Error::format("failed to find validator {} to remove", to_hex(address));
      removed_voting_power += val->voting_power;
    }
    if (deletes.size() > validators.size()) {
      throw std::runtime_error("more deletes than validators");
    }

    // Verify that applying the 'updates' against 'vals' will not result in error.
    // Get the updated total voting power before removal. Note that this is < 2 * MaxTotalVotingPower
    auto delta = [](validator& update, const std::shared_ptr<validator_set>& vals) {
      auto val = vals->get_by_address(update.address);
      if (val.has_value())
        return update.voting_power - val->voting_power;
      return update.voting_power;
    };
    std::vector<validator> updatesCopy(updates);
    sort(updatesCopy.begin(), updatesCopy.end(),
      [delta, this](validator a, validator b) { return delta(a, shared_from_this()) < delta(b, shared_from_this()); });
    auto tvp_after_removals = total_voting_power - removed_voting_power;
    for (auto& val_update : updatesCopy) {
      tvp_after_removals += delta(val_update, shared_from_this());
      if (tvp_after_removals > max_total_voting_power)
        return Error::format("total voting power of resulting valset exceeds max");
    }
    auto tvp_after_updates_before_removals = tvp_after_removals + removed_voting_power;

    // Compute the priorities for updates.
    compute_new_priorities(updates, tvp_after_updates_before_removals);

    // Apply updates and removals.
    apply_updates(updates);
    apply_removals(deletes);

    update_total_voting_power();

    // Scale and center.
    rescale_priorities(priority_window_size_factor * get_total_voting_power());
    shift_by_avg_proposer_priority();

    sort(validators.begin(), validators.end(), [](validator a, validator b) {
      if (a.voting_power == b.voting_power)
        return a.address < b.address;
      return a.voting_power > b.voting_power;
    });
    return success();
  }

  /** \brief computes the proposer priority for the validators not present in the set based on
   * 'updated_total_voting_power'.
   * Leaves unchanged the priorities of validators that are changed.
   *
   * 'updates' parameter must be a list of unique validators to be added or updated.
   *
   * 'updated_total_voting_power' is the total voting power of a set where all updates would be applied but
   *   not the removals. It must be < 2*MaxTotalVotingPower and may be close to this limit if close to
   *   MaxTotalVotingPower will be removed. This is still safe from overflow since MaxTotalVotingPower is maxInt64/8.
   *
   * No changes are made to the validator set 'vals'.
   */
  void compute_new_priorities(const std::vector<validator>& updates, int64_t updated_total_voting_power) {
    for (auto val_update : updates) {
      auto address = val_update.address;
      auto val = get_by_address(address);
      if (!val.has_value()) {
        // add val
        // Set ProposerPriority to -C*totalVotingPower (with C ~= 1.125) to make sure validators can't
        // un-bond and then re-bond to reset their (potentially previously negative) ProposerPriority to zero.
        //
        // Contract: updatedVotingPower < 2 * MaxTotalVotingPower to ensure ProposerPriority does
        // not exceed the bounds of int64.
        //
        // Compute ProposerPriority = -1.125*totalVotingPower == -(updatedVotingPower + (updatedVotingPower >> 3)).
        val_update.proposer_priority = -(updated_total_voting_power + (updated_total_voting_power >> 3));
      } else {
        val_update.proposer_priority = val->proposer_priority;
      }
    }
  }

  std::shared_ptr<validator_set> copy_increment_proposer_priority(int32_t times) {
    auto copy_ = this->copy();
    copy_->increment_proposer_priority(times);
    return copy_;
  }

  void increment_proposer_priority(int32_t times) {
    if (validators.empty())
      throw std::runtime_error("empty validator set");
    if (times <= 0)
      throw std::runtime_error("cannot call with non-positive times");

    // Cap the difference between priorities to be proportional to 2*totalPower by
    // re-normalizing priorities, i.e., rescale all priorities by multiplying with:
    //  2*totalVotingPower/(maxPriority - minPriority)
    auto diff_max = priority_window_size_factor * get_total_voting_power();
    rescale_priorities(diff_max);
    shift_by_avg_proposer_priority();

    for (auto i = 0; i < times; i++) {
      for (auto& val : validators) {
        val.proposer_priority += val.voting_power; // todo - check safe add
      }
      // find validator with most priority
      auto it = std::max_element(validators.begin(), validators.end(),
        [](const validator& a, const validator& b) { return a.proposer_priority < b.proposer_priority; });
      it->proposer_priority -= total_voting_power;
      proposer = *it;
    }
  }

  /** \brief rescales the priorities such that the distance between the
   * maximum and minimum is smaller than `diffMax`. throws if validator set is empty.
   */
  void rescale_priorities(int64_t diff_max) {
    if (validators.empty())
      throw std::runtime_error("empty validator set");
    if (diff_max <= 0)
      return;

    // Calculating ceil(diff/diffMax):
    // Re-normalization is performed by dividing by an integer for simplicity.
    auto max = std::numeric_limits<int64_t>::max();
    auto min = std::numeric_limits<int64_t>::min();
    for (const auto& val : validators) {
      if (val.proposer_priority < min)
        min = val.proposer_priority;
      if (val.proposer_priority > max)
        max = val.proposer_priority;
    }
    auto diff = max - min;
    if (diff < 0)
      diff = -diff;
    auto ratio = (diff + diff_max - 1) / diff_max;
    if (diff > diff_max) {
      for (auto& val : validators) {
        val.proposer_priority /= ratio;
      }
    }
  }

  void shift_by_avg_proposer_priority() {
    if (validators.empty())
      throw std::runtime_error("empty validator set");
    // compute average proposer_priority
    int64_t sum = 0;
    for (auto& val : validators) {
      sum += val.proposer_priority;
    }
    if (sum > 0) {
      int64_t avg = sum / validators.size();
      for (auto& val : validators) {
        val.proposer_priority -= avg; // todo - check safe sub
      }
    }
  }

  Result<void> validate_basic() {
    return success(); // TODO
  }

  static std::unique_ptr<::tendermint::types::ValidatorSet> to_proto(const std::shared_ptr<validator_set>& v) {
    auto ret = std::make_unique<::tendermint::types::ValidatorSet>();
    if (v->validators.empty())
      return ret;
    auto vals_proto = ret->mutable_validators();
    for (auto& val : v->validators)
      vals_proto->AddAllocated(validator::to_proto(val).release());
    if (v->proposer.has_value())
      ret->set_allocated_proposer(validator::to_proto(v->proposer.value()).release());
    ret->set_total_voting_power(0);
    return ret;
  }

  static Result<std::shared_ptr<validator_set>> from_proto(const ::tendermint::types::ValidatorSet& pb) {
    auto ret = validator_set::new_validator_set({});
    for (auto& v : pb.validators()) {
      if (auto ok = validator::from_proto(v); !ok)
        return ok.error();
      else
        ret->validators.push_back(*ok.value());
    }
    if (auto ok = validator::from_proto(pb.proposer()); !ok)
      return Error::format("from_proto failed: {}", ok.error().message());
    else
      ret->proposer = *ok.value();
    ret->get_total_voting_power();
    if (auto ok = ret->validate_basic(); !ok)
      return ok.error();
    return ret;
  }

  Result<void> verify_commit_light(const std::string& chain_id,
    p2p::block_id block_id_,
    int64_t height,
    const std::shared_ptr<struct commit>& commit_);
  Result<void> verify_commit_light_trusting(const std::string& chain_id, const std::shared_ptr<struct commit>& commit_);
};

} // namespace noir::consensus

NOIR_REFLECT(noir::consensus::validator, address, pub_key_, voting_power, proposer_priority);
NOIR_REFLECT(noir::consensus::validator_set, validators, proposer, total_voting_power);
