// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/common/plugin_interface.h>
#include <noir/common/thread_pool.h>
#include <noir/consensus/block_executor.h>
#include <noir/consensus/common.h>
#include <noir/consensus/config.h>
#include <noir/consensus/crypto.h>
#include <noir/consensus/state.h>
#include <noir/consensus/types/event_bus.h>
#include <noir/consensus/types/node_id.h>
#include <noir/consensus/types/priv_validator.h>
#include <noir/consensus/types/protobuf.h>
#include <noir/consensus/types/round_state.h>
#include <noir/consensus/wal.h>

namespace noir::consensus {

/**
 * Handles execution of the consensus algorithm.
 * It processes votes and proposals, and upon reaching agreement,
 * commits blocks to the chain and executes them against the application.
 * The internal state machine receives input from peers, the internal validator, and from a timer.
 */
struct consensus_state : public std::enable_shared_from_this<consensus_state> {
  consensus_state(appbase::application&, const std::shared_ptr<events::event_bus>&);

  static std::shared_ptr<consensus_state> new_state(appbase::application& app,
    const consensus_config& cs_config_,
    state& state_,
    const std::shared_ptr<block_executor>& block_exec_,
    const std::shared_ptr<block_store>& new_block_store,
    const std::shared_ptr<ev::evidence_pool>& new_ev_pool,
    const std::shared_ptr<events::event_bus>& event_bus_);

  state get_state();
  int64_t get_last_height();
  std::shared_ptr<round_state> get_round_state();
  void set_priv_validator(const std::shared_ptr<priv_validator>& priv);
  void update_priv_validator_pub_key();
  void reconstruct_last_commit(state& state_);

  /// \brief LoadCommit loads the commit for a given height.
  /// \param[in] height
  /// \return shared_ptr of commit
  std::shared_ptr<commit> load_commit(int64_t height);
  void on_start();
  void on_stop();

  void update_height(int64_t height);
  void update_round_step(int32_t rount, p2p::round_step_type step);
  void schedule_round_0(round_state& rs_);
  void update_to_state(state& state_);
  void new_step();

  void receive_routine(p2p::internal_msg_info_ptr mi);
  void handle_msg();

  void schedule_timeout(
    std::chrono::system_clock::duration duration_, int64_t height, int32_t round, p2p::round_step_type step);
  void tick(timeout_info_ptr ti);
  void tock(timeout_info_ptr ti);
  void handle_timeout(timeout_info_ptr ti);

  void enter_new_round(int64_t height, int32_t round);

  /// \brief needProofBlock returns true on the first height (so the genesis app hash is signed right away) and where
  /// the last block (height-1) caused the app hash to change
  /// \param[in] height
  /// \return true if proof block is needed, false otherwise
  bool need_proof_block(int64_t height);

  void enter_propose(int64_t height, int32_t round);
  bool is_proposal_complete();
  bool is_proposal(Bytes address);
  void decide_proposal(int64_t height, int32_t round);

  void enter_prevote(int64_t height, int32_t round);
  void do_prevote(int64_t height, int32_t round);

  void enter_prevote_wait(int64_t height, int32_t round);
  void enter_precommit(int64_t height, int32_t round);
  void enter_precommit_wait(int64_t height, int32_t round);
  void enter_commit(int64_t height, int32_t round);

  void try_finalize_commit(int64_t height);
  void finalize_commit(int64_t height);
  void set_proposal(p2p::proposal_message& msg);
  bool add_proposal_block_part(p2p::block_part_message& msg, node_id peer_id);

  /// \brief attempt to add vote; if it's a duplicate signature, dupeout the validator
  Result<bool> try_add_vote(p2p::vote_message& msg, const node_id& peer_id);
  std::pair<bool, Error> add_vote(const std::shared_ptr<vote>& vote_, const node_id& peer_id);
  std::optional<vote> sign_vote(p2p::signed_msg_type msg_type, Bytes hash, p2p::part_set_header header);
  tstamp vote_time();
  vote sign_add_vote(p2p::signed_msg_type msg_type, Bytes hash, p2p::part_set_header header);

  consensus_config cs_config;

  //  privValidator     types.PrivValidator // for signing votes
  //  privValidatorType types.PrivValidatorType
  std::shared_ptr<priv_validator> local_priv_validator;
  priv_validator_type local_priv_validator_type;

  std::shared_ptr<block_store> block_store_{};
  std::shared_ptr<block_executor> block_exec{};

  // notify us if txs are available
  //  txNotifier txNotifier

  std::shared_ptr<ev::evidence_pool> ev_pool{};

  // internal state
  std::mutex mtx;
  round_state rs{};
  state local_state; // State until height-1.
  pub_key local_priv_validator_pub_key;

  ///< no need for peer_mq; we have one at consensus_reactor which handles all messages from peers

  ///-- directly implemented decide_proposal
  ///-- directly implemented do_prevote
  ///-- directly implemented set_proposal

  plugin_interface::egress::channels::event_switch_message_queue::channel_type& event_switch_mq_channel;

  // internalMsgQueue chan msgInfo
  plugin_interface::channels::internal_message_queue::channel_type& internal_mq_channel;
  plugin_interface::channels::internal_message_queue::channel_type::handle internal_mq_subscription;

  // timeoutTicker TimeoutTicker
  plugin_interface::channels::timeout_ticker::channel_type& timeout_ticker_channel;
  plugin_interface::channels::timeout_ticker::channel_type::handle timeout_ticker_subscription;
  std::mutex timeout_ticker_mtx;
  std::unique_ptr<boost::asio::steady_timer> timeout_ticker_timer;
  uint16_t thread_pool_size = 2;
  std::optional<named_thread_pool> thread_pool;
  timeout_info_ptr old_ti;

  int n_steps{}; // for tests where we want to limit the number of transitions the state makes

  // we use eventBus to trigger msg broadcasts in the reactor,
  // and to notify external subscribers, eg. through a websocket
  std::shared_ptr<events::event_bus> event_bus_;

  // a Write-Ahead Log ensures we can recover from any kind of crash and helps us avoid signing conflicting votes
  // TODO: configurable
  static constexpr size_t wal_file_num = 1024;
  static constexpr size_t wal_file_size = 1024 * 1024;
  std::string wal_head_name = "wal";

  std::unique_ptr<wal> wal_;

  /// \brief load configured wal file
  /// \return true on success, false otherwise
  bool load_wal_file();

  bool do_wal_catchup = false; ///< determines if we even try to do the catchup
  bool replay_mode = false; ///< so we don't log signing errors during replay

  /// \brief Replay only those messages since the last block. `timeoutRoutine` should run concurrently to read off
  /// tickChan.
  /// \param[in] cs_height
  /// \return true on success, false otherwise
  bool catchup_replay(int64_t cs_height);

  /// \brief Functionality to replay blocks and messages on recovery from a crash.
  /// There are two general failure scenarios:
  ///   1. failure during consensus
  ///   2. failure while applying the block
  ///     The former is handled by the WAL, the latter by the proxyApp Handshake on
  ///     restart, which ultimately hands off the work to the WAL.
  ///
  /// //-----------------------------------------
  /// // 1. Recover from failure during consensus
  /// // (by replaying messages from the WAL)
  /// //-----------------------------------------
  /// Unmarshal and apply a single message to the consensus state as if it were
  /// received in receiveRoutine.  Lines that start with "#" are ignored.
  /// \note receiveRoutine should not be running.
  /// \param[in] msg
  /// \return true on success, false otherwise
  bool read_replay_message(const timed_wal_message& msg);
};

/// //---------------------------------------------------
/// // 2. Recover from failure while applying the block.
/// // (by handshaking with the app to figure out where
/// //  we were last, and using the WAL to recover there)
/// //---------------------------------------------------
struct handshaker {
  std::shared_ptr<block_store> block_store_{};
  state& initial_state;
  std::shared_ptr<noir::consensus::db_store> state_store{};
  std::shared_ptr<events::event_bus> event_bus_{};
  std::shared_ptr<genesis_doc> gen_doc{};

  int n_blocks{};

  static std::shared_ptr<handshaker> new_handshaker(const std::shared_ptr<block_store>& b_store_,
    state& i_state,
    const std::shared_ptr<noir::consensus::db_store>& s_store,
    const std::shared_ptr<events::event_bus>& e_bus_,
    const std::shared_ptr<genesis_doc>& g_doc) {
    return std::make_shared<handshaker>(handshaker{.block_store_ = b_store_,
      .initial_state = i_state,
      .state_store = s_store,
      .event_bus_ = e_bus_,
      .gen_doc = g_doc,
      .n_blocks = 0});
  }

  Result<void> handshake(const std::shared_ptr<app_connection>& proxy_app) {
    if (proxy_app->is_socket) {
      tendermint::abci::RequestInfo req;
      req.set_version("0.35.6");
      req.set_block_version(11);
      req.set_p2p_version(8);
      req.set_abci_version("0.17.0");
      auto res = proxy_app->application->info_sync(req);
      if (!res)
        return Error::format("ABCI failed: info_sync");

      auto block_height = res->last_block_height();
      if (block_height < 0)
        return Error::format("got a negative last_block_height from app");
      auto app_hash = to_hex(res->last_block_app_hash());
      ilog(fmt::format("ABCI Handshake App Info: height={} hash={} software-version={} protocol-version={}",
        block_height, app_hash, res->version(), res->app_version()));

      if (initial_state.last_block_height == 0)
        initial_state.version.cs.app = res->app_version();

      // Replay blocks up to latest in block_store
      replay_blocks(initial_state, from_hex(app_hash), block_height, proxy_app);

      ilog(fmt::format(
        "Completed ABCI Handshake - Tendermint and App are synced: app_height={} app_hash={}", block_height, app_hash));
    }
    return success();
  }

  Result<Bytes> replay_blocks(
    state& state_, const Bytes& app_hash, int64_t app_block_height, const std::shared_ptr<app_connection>& proxy_app) {
    auto store_block_base = block_store_->base();
    auto store_block_height = block_store_->height();
    auto state_block_height = state_.last_block_height;
    ilog(fmt::format("ABCI Replay Blocks : app_height={} store_height={} state_height={}", app_block_height,
      store_block_height, state_block_height));

    if (app_block_height == 0) {
      std::vector<validator> validators;
      for (auto& v : gen_doc->validators)
        validators.push_back(validator::new_validator(v.pub_key, v.power));
      auto val_set = validator_set::new_validator_set(validators);
      auto next_vals = tm2pb::validator_updates(val_set);
      auto pb_params = consensus_params::to_proto(gen_doc->cs_params.value());
      tendermint::abci::RequestInitChain req;
      *req.mutable_time() = ::google::protobuf::util::TimeUtil::MicrosecondsToTimestamp(gen_doc->genesis_time);
      req.set_chain_id(gen_doc->chain_id);
      req.set_initial_height(gen_doc->initial_height);
      req.set_allocated_consensus_params(pb_params.release());
      auto pb_vals = req.mutable_validators();
      for (auto& val : next_vals)
        *req.mutable_validators()->Add() = val;
      req.set_app_state_bytes({gen_doc->app_state.begin(), gen_doc->app_state.end()});

      auto res = proxy_app->application->init_chain(req);
      if (!res)
        return Error::format("ABCI failed: init_chain");
      auto new_app_hash = from_hex(to_hex(res->app_hash()));

      if (state_block_height == 0) {
        if (!new_app_hash.empty())
          state_.app_hash = new_app_hash;
        if (!res->validators().empty()) {
          auto vals = pb2tm::validator_updates(res->validators()).value();
          state_.validators = validator_set::new_validator_set(vals);
          state_.next_validators = validator_set::new_validator_set(vals)->copy_increment_proposer_priority(1);
        } else if (gen_doc->validators.empty()) {
          return Error::format("validator set is nil in genesis and still empty after InitChain");
        }
        if (res->consensus_params().IsInitialized()) {
          // TODO : implement
        }

        // Update last_result_hash with empty hash, conforming to RFC-6962
        state_.last_result_hash = merkle::get_empty_hash();
        if (!state_store->save(state_))
          return Error::format("replay_blocks failed: could not save");
      }
    }

    if (store_block_height == 0)
      return app_hash;
    if (app_block_height == 0 && state_.initial_height < store_block_base)
      return Error::format("app_block_height is too low");
    if (app_block_height > 0 && app_block_height < store_block_base - 1)
      return Error::format("app_block_height is too low");
    if (store_block_height < app_block_height)
      return Error::format("app_block_height is too low");
    if (store_block_height < state_block_height)
      check(false, "state_block_height > store_block_height");
    if (store_block_height > state_block_height + 1)
      check(false, "store_block_height > state_block_height + 1");

    if (store_block_height == state_block_height) {
      if (app_block_height < store_block_height) {
        // return replay_blocks_internal(); // TODO : implement
      } else if (app_block_height == store_block_height) {
        return app_hash;
      }
    }

    return app_hash; // TODO : remove this and continue implementation
  }
};

/// \brief repair wal file until first error is encountered
/// \param[in] src path to corrupt file
/// \param[in] dst path to new wal file
/// \return true on success, false otherwise
bool repair_wal_file(const std::string& src, const std::string& dst);

} // namespace noir::consensus

NOIR_REFLECT(noir::consensus::consensus_state, local_state, n_steps);
