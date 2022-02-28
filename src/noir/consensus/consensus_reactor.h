// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/consensus/consensus_state.h>
#include <noir/consensus/peer_state.h>
#include <noir/consensus/state.h>
#include <noir/consensus/store/store_test.h>
#include <noir/consensus/types.h>

namespace noir::consensus {

struct consensus_reactor {

  std::shared_ptr<consensus_state> cs_state;

  std::mutex mtx;
  std::map<std::string, std::shared_ptr<peer_state>> peers;
  bool wait_sync;

  // Receive an event from consensus_state
  plugin_interface::egress::channels::event_switch_message_queue::channel_type::handle event_switch_mq_subscription =
    appbase::app().get_channel<plugin_interface::egress::channels::event_switch_message_queue>().subscribe(
      std::bind(&consensus_reactor::process_event, this, std::placeholders::_1));

  // Receive an envelope from peers [via p2p]
  plugin_interface::incoming::channels::receive_message_queue::channel_type::handle recv_mq_subscription =
    appbase::app().get_channel<plugin_interface::incoming::channels::receive_message_queue>().subscribe(
      std::bind(&consensus_reactor::process_peer_msg, this, std::placeholders::_1));

  // Send an envelope to peers [via p2p]
  plugin_interface::egress::channels::transmit_message_queue::channel_type& xmt_mq_channel;

  consensus_reactor(std::shared_ptr<consensus_state> new_cs_state, bool new_wait_sync)
    : cs_state(std::move(new_cs_state)), wait_sync(new_wait_sync),
      xmt_mq_channel(appbase::app().get_channel<plugin_interface::egress::channels::transmit_message_queue>()) {}

  static std::shared_ptr<consensus_reactor> new_consensus_reactor(
    std::shared_ptr<consensus_state>& new_cs_state, bool new_wait_sync) {
    auto consensus_reactor_ = std::make_shared<consensus_reactor>(new_cs_state, new_wait_sync);
    return consensus_reactor_;
  }

  void on_start() {
    cs_state->on_start();
  }

  void on_stop() {}

  void process_event(const plugin_interface::event_info_ptr& info) {
    switch (info->event_) {
    case EventNewRoundStep:
      broadcast_new_round_step_message(std::get<round_state>(info->message_));
      break;
    case EventValidBlock:
      broadcast_new_valid_block_message(std::get<round_state>(info->message_));
      break;
    case EventVote:
      broadcast_has_vote_message(std::get<p2p::vote_message>(info->message_));
      break;
    }
  }

  void process_peer_msg(p2p::envelope_ptr info);

  std::shared_ptr<peer_state> get_peer_state(std::string peer_id) {
    std::lock_guard<std::mutex> g(mtx);
    auto it = peers.find(peer_id);
    return it == peers.end() ? nullptr : it->second;
  }

  void broadcast_new_round_step_message(const round_state& rs) {
    auto msg = make_round_step_message(rs);
    transmit_new_envelope("", "", msg, true);
  }

  void broadcast_new_valid_block_message(const round_state& rs) {
    auto msg = p2p::new_valid_block_message{rs.height, rs.round,
      rs.proposal_block_parts->header() /* TODO: check if this is correct*/, rs.proposal_block_parts->get_bit_array(),
      rs.step == p2p::round_step_type::Commit};
    transmit_new_envelope("", "", msg, true);
  }

  void broadcast_has_vote_message(const p2p::vote_message& vote_) {
    auto msg = p2p::has_vote_message{vote_.height, vote_.round, vote_.type, vote_.validator_index};
    transmit_new_envelope("", "", msg, true);
  }

  p2p::new_round_step_message make_round_step_message(const round_state& rs) {
    return p2p::new_round_step_message{
      rs.height, rs.round, rs.step, rs.start_time /* TODO: find elapsed seconds*/, rs.last_commit->round};
  }

  void transmit_new_envelope(std::string from, std::string to, const p2p::reactor_message& msg, bool broadcast = false,
    int priority = appbase::priority::medium);
};

} // namespace noir::consensus
