// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/consensus/block_executor.h>
#include <noir/consensus/block_sync/block_pool.h>
#include <noir/consensus/consensus_reactor.h>
#include <noir/consensus/state.h>

namespace noir::consensus::block_sync {

constexpr auto try_sync_interval{std::chrono::milliseconds(10)};
constexpr auto status_update_interval{std::chrono::seconds(10)};
constexpr auto switch_to_consensus_interval{std::chrono::seconds(1)};
constexpr auto sync_timeout{std::chrono::seconds(60)};

struct reactor {
  // Immutable
  consensus::state initial_state;

  std::shared_ptr<consensus::block_executor> block_exec;
  std::shared_ptr<consensus::block_store> store;
  std::shared_ptr<block_pool> pool;

  std::atomic_bool block_sync;

  p2p::tstamp sync_start_time;

  // Receive an envelope from peers [via p2p]
  plugin_interface::incoming::channels::bs_reactor_message_queue::channel_type::handle bs_reactor_mq_subscription =
    appbase::app().get_channel<plugin_interface::incoming::channels::bs_reactor_message_queue>().subscribe(
      std::bind(&reactor::process_peer_msg, this, std::placeholders::_1));

  // Send an envelope to peers [via p2p]
  plugin_interface::egress::channels::transmit_message_queue::channel_type& xmt_mq_channel =
    appbase::app().get_channel<plugin_interface::egress::channels::transmit_message_queue>();

  static std::shared_ptr<reactor> new_reactor(state& state_, const std::shared_ptr<block_executor>& block_exec_,
    const std::shared_ptr<block_store>& new_block_store, bool block_sync_) {

    if (state_.last_block_height != new_block_store->height()) {
      elog("block_sync_reactor: state_ and store height mismatch");
      return {};
    }

    auto start_height = new_block_store->height() + 1;
    if (start_height == 1)
      start_height = state_.initial_height;

    auto reactor_ = std::make_shared<reactor>();
    reactor_->initial_state = state_;
    reactor_->block_exec = block_exec_;
    reactor_->store = new_block_store;
    reactor_->pool = block_pool::new_block_pool(start_height);
    reactor_->block_sync = block_sync_;
    reactor_->sync_start_time = 0;

    return reactor_;
  }

  void on_start() {
    if (block_sync.load()) {
      pool->on_start();
      // pool_routine // TODO
    }
    process_block_sync_ch();
    process_peer_updates();
  }

  void on_stop() {
    if (block_sync.load()) {
      pool->on_stop();
    }
    // TODO: close channel reads
  }

  void process_peer_msg(p2p::envelope_ptr info);

  void process_block_sync_ch() {}

  void process_peer_updates() {}

  void respond_to_peer(std::shared_ptr<consensus::block_request> msg, const std::string& peer_id);

  std::optional<std::string> switch_to_block_sync(const state& state_) {
    block_sync.store(true);
    initial_state = state_;
    pool->height = state_.last_block_height + 1;

    pool->on_start();

    sync_start_time = get_time();

    pool_routine();

    return {};
  }

  // void request_routine() // TODO

  void pool_routine() {
    // TODO
  }

  int64_t get_max_peer_block_height() {
    return pool->max_peer_height;
  }

  boost::asio::steady_timer::duration get_total_synced_time() {
    // TODO
    return std::chrono::seconds(10);
  }

  boost::asio::steady_timer::duration get_remaining_sync_time() {
    // TODO
    return std::chrono::seconds(10);
  }

  void transmit_new_envelope(const std::string& from, const std::string& to, const p2p::bs_reactor_message& msg,
    bool broadcast = false, int priority = appbase::priority::medium);
};

} // namespace noir::consensus::block_sync
