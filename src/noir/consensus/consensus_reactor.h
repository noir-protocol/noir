// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma
#include <noir/consensus/consensus_state.h>
#include <noir/consensus/state.h>
#include <noir/consensus/store/store_test.h>
#include <noir/consensus/types.h>

namespace noir::consensus {

class consensus_reactor : boost::noncopyable {
public:
  consensus_reactor();

  static std::unique_ptr<consensus_reactor> new_consensus_reactor(
    const config& local_config, state& prev_state, priv_validator priv);

  void on_start();

  void on_stop();

  void GetPeerState();

  void broadcastNewRoundStepMessage();

  void broadcastNewValidBlockMessage();

  void broadcastHasVoteMessage();

  void makeRoundStepMessage();

  void sendNewRoundStepMessage();

  void gossipVotesForHeight();

  void handleStateMessage();

  void handleVoteMessage();

  void handleMessage();

private:
  //  state    *State
  std::shared_ptr<consensus_state> local_state;

  //  eventBus *types.EventBus
  //  Metrics  *Metrics
  //
  //  mtx      tmsync.RWMutex
  //  peers    map[types.NodeID]*PeerState
  //  waitSync bool
  //
  //  stateCh       *p2p.Channel
  //  dataCh        *p2p.Channel
  //  voteCh        *p2p.Channel
  //  voteSetBitsCh *p2p.Channel
  //  peerUpdates   *p2p.PeerUpdates
};

std::unique_ptr<consensus_reactor> consensus_reactor::new_consensus_reactor(
  const config& local_config, state& prev_state, priv_validator priv) {
  auto consensus_reactor_ = std::make_unique<consensus_reactor>();

  auto session = std::make_shared<noir::db::session::session<noir::db::session::rocksdb_t>>(make_session());
  auto dbs = std::make_shared<noir::consensus::db_store>(session);
  auto proxyApp = std::make_shared<app_connection>();
  auto bls = std::make_shared<noir::consensus::block_store>(session);
  auto block_exec = block_executor::new_block_executor(dbs, proxyApp, bls);

  auto cs = consensus_state::new_state(local_config.consensus, prev_state, block_exec, bls);
  if (local_config.base.mode == "validator")
    cs->set_priv_validator(priv);

  consensus_reactor_->local_state = move(cs);

  return consensus_reactor_;
}

} // namespace noir::consensus
