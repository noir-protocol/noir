// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <noir/common/log.h>
#include <noir/consensus/block_sync/reactor.h>
#include <noir/consensus/ev/reactor.h>
#include <noir/consensus/node.h>

namespace noir::consensus {

std::unique_ptr<node> node::new_default_node(appbase::application& app, const std::shared_ptr<config>& new_config) {
  // Load or generate priv
  std::vector<genesis_validator> validators;
  std::vector<std::shared_ptr<priv_validator>> priv_validators;
  std::filesystem::path pv_root_dir = new_config->priv_validator.root_dir;
  auto priv_val = noir::consensus::privval::file_pv::load_or_gen_file_pv(
    pv_root_dir / new_config->priv_validator.key, pv_root_dir / new_config->priv_validator.state);
  if (!priv_val)
    check(false, priv_val.error().message());

  auto vote_power = 10;
  auto val = validator{priv_val.value()->get_address(), priv_val.value()->get_pub_key(), vote_power, 0};
  validators.push_back(genesis_validator{val.address, val.pub_key_, val.voting_power});
  priv_validators.push_back(std::move(priv_val.value()));

  std::shared_ptr<genesis_doc> gen_doc{};
  if (auto ok = genesis_doc::genesis_doc_from_file(new_config->consensus.root_dir + "/config/genesis.json"); !ok) {
    wlog("Unable to load genesis from json.file. Will load default genesis.");
    gen_doc = std::make_shared<genesis_doc>(genesis_doc{get_time(), new_config->base.chain_id, 1, {}, validators});
    // gen_doc->save(new_config->consensus.root_dir + "/config/genesis.json");
  } else {
    gen_doc = ok.value();
  }

  // Load or generate node_key
  auto node_key_dir = std::filesystem::path{new_config->consensus.root_dir} / "config";
  auto node_key_ = node_key::load_or_gen_node_key(node_key_dir / new_config->base.node_key);

  auto db_dir = std::filesystem::path{new_config->consensus.root_dir} / std::string(default_data_dir);
  auto session = make_session(false, db_dir);

  return make_node(app, new_config, priv_validators[0], node_key_, gen_doc, session);
}

std::unique_ptr<node> node::make_node(appbase::application& app,
  const std::shared_ptr<config>& new_config,
  const std::shared_ptr<priv_validator>& new_priv_validator,
  const std::shared_ptr<node_key>& new_node_key,
  const std::shared_ptr<genesis_doc>& new_genesis_doc,
  const std::shared_ptr<noir::db::session::session<noir::db::session::rocksdb_t>>& session) {

  auto dbs = std::make_shared<noir::consensus::db_store>(session);
  auto proxy_app = create_and_start_proxy_app(new_config->base.proxy_app);
  auto bls = std::make_shared<noir::consensus::block_store>(session);
  auto ev_bus = std::make_shared<noir::consensus::events::event_bus>(app);

  state state_ = load_state_from_db_or_genesis(dbs, new_genesis_doc);

  // EventBus and IndexerService must be started before the handshake because
  // we might need to index the txs of the replayed block as this might not have happened
  // when the node stopped last time (i.e. the node stopped after it saved the block
  // but before it indexed the txs, or, endblocker panicked)
  auto event_bus_ = std::make_shared<events::event_bus>(app);
  // indexer
  auto event_sinks_ = indexer::sink::event_sink_from_config(new_config);
  if (!event_sinks_)
    check(false, fmt::format("unable to start node: check event_sink {}", event_sinks_.error().message()));
  auto indexer_service_ = std::make_shared<indexer::indexer_service>(event_sinks_.value(), event_bus_);
  indexer_service_->on_start();

  // Setup pub_key_
  pub_key pub_key_;
  if (new_config->base.mode == Validator) {
    pub_key_ = new_priv_validator->get_pub_key();
    // todo - check error
  }

  // Setup state_sync [use snapshots to replicate states]
  bool new_state_sync_on = false; ///< noir will not implement state_sync

  // Setup block_sync
  bool block_sync = new_config->base.fast_sync_mode;

  // Create handshaker
  auto handshaker_ = handshaker::new_handshaker(bls, state_, dbs, event_bus_, new_genesis_doc);
  handshaker_->handshake(proxy_app);

  log_node_startup_info(state_, pub_key_, new_config->base.mode);

  auto ok_ev_reactor = create_evidence_reactor(app, new_config, session, bls);
  if (!ok_ev_reactor)
    check(false, fmt::format("unable to start node: {}", ok_ev_reactor.error().message()));
  auto [new_ev_reactor, new_ev_pool] = ok_ev_reactor.value();

  auto block_exec = block_executor::new_block_executor(dbs, proxy_app, new_ev_pool, bls, ev_bus);

  auto [new_cs_reactor, new_cs_state] = create_consensus_reactor(app, new_config, std::make_shared<state>(state_),
    block_exec, bls, new_ev_pool, new_priv_validator, event_bus_, block_sync);

  auto new_bs_reactor = create_block_sync_reactor(app, state_, block_exec, bls, block_sync);

  // Setup callbacks // TODO: is this right place to setup callback?
  new_bs_reactor->set_callback_switch_to_cs_sync(
    std::bind(&consensus_reactor::switch_to_consensus, new_cs_reactor, std::placeholders::_1, std::placeholders::_2));

  auto node_ = std::make_unique<node>();
  node_->config_ = new_config;
  node_->genesis_doc_ = new_genesis_doc;
  node_->priv_validator_ = new_priv_validator;
  node_->node_key_ = new_node_key;
  node_->store_ = dbs;
  node_->block_store_ = bls;
  node_->event_bus_ = event_bus_;
  node_->event_sink_ = event_sinks_.value();
  node_->indexer_service_ = indexer_service_;
  node_->state_sync_on = new_state_sync_on;
  node_->bs_reactor = new_bs_reactor;
  node_->cs_reactor = new_cs_reactor;
  node_->ev_reactor = new_ev_reactor;
  return node_;
}

std::shared_ptr<app_connection> node::create_and_start_proxy_app(const std::string& app_name) {
  auto proxy_app = std::make_shared<app_connection>(app_name);
  // handle all errors here; if fails throw exception
  proxy_app->start();
  return proxy_app;
}

void node::log_node_startup_info(state& state_, pub_key& pub_key_, node_mode mode) {
  ilog(fmt::format("Version info: version={}, mode={}", state_.version.software, mode_str(mode)));
  switch (mode) {
  case Full:
    ilog("################################");
    ilog("### This node is a full_node ###");
    ilog("################################");
    break;
  case Validator:
    ilog("#####################################");
    ilog("### This node is a validator_node ###");
    ilog("#####################################");
    {
      auto addr = pub_key_.address();
      if (state_.validators->has_address(addr))
        ilog("   > node is in the active validator set");
      else
        ilog("   > node is NOT in the active validator set");
    }
    break;
  case Seed:
    ilog("################################");
    ilog("### This node is a seed_node ###");
    ilog("################################");
    break;
  case Unknown:
    ilog("#################################");
    ilog("### This node is unknown_mode ###");
    ilog("#################################");
    break;
  }
}

std::shared_ptr<block_sync::reactor> node::create_block_sync_reactor(appbase::application& app,
  state& state_,
  const std::shared_ptr<block_executor>& block_exec_,
  const std::shared_ptr<block_store>& new_block_store,
  bool block_sync_) {
  auto bs_reactor = block_sync::reactor::new_reactor(app, state_, block_exec_, new_block_store, block_sync_);
  return bs_reactor;
}

Result<std::tuple<std::shared_ptr<ev::reactor>, std::shared_ptr<ev::evidence_pool>>> node::create_evidence_reactor(
  appbase::application& app,
  const std::shared_ptr<config>& new_config,
  const std::shared_ptr<noir::db::session::session<noir::db::session::rocksdb_t>>& session,
  const std::shared_ptr<block_store>& new_block_store) {
  auto db_dir = std::filesystem::path{new_config->consensus.root_dir} / "data/evidence.db"; // TODO : clean up
  auto evidence_session = make_session(false, db_dir);

  auto state_store = std::make_shared<noir::consensus::db_store>(session);

  auto evidence_pool = ev::evidence_pool::new_pool(evidence_session, state_store, new_block_store);
  if (!evidence_pool)
    return Error::format("unable to create evidence pool: {}", evidence_pool.error().message());

  auto evidence_reactor = ev::reactor::new_reactor(app, evidence_pool.value());

  return {evidence_reactor, evidence_pool.value()};
}

std::tuple<std::shared_ptr<consensus_reactor>, std::shared_ptr<consensus_state>> node::create_consensus_reactor(
  appbase::application& app,
  const std::shared_ptr<config>& config_,
  const std::shared_ptr<state>& state_,
  const std::shared_ptr<block_executor>& block_exec_,
  const std::shared_ptr<block_store>& block_store_,
  const std::shared_ptr<ev::evidence_pool>& ev_pool_,
  const std::shared_ptr<priv_validator>& priv_validator_,
  const std::shared_ptr<events::event_bus>& event_bus_,
  bool wait_sync) {
  auto cs_state =
    consensus_state::new_state(app, config_->consensus, *state_, block_exec_, block_store_, ev_pool_, event_bus_);

  if (config_->base.mode == Validator)
    cs_state->set_priv_validator(priv_validator_);

  auto cs_reactor = consensus_reactor::new_consensus_reactor(app, cs_state, event_bus_, wait_sync);

  return {cs_reactor, cs_state};
}

void node::on_start() {
  // Check genesis time and sleep until time is ready
  auto initial_sleep_duration = std::chrono::microseconds(genesis_doc_->genesis_time - get_time());
  if (initial_sleep_duration.count() > 0) {
    ilog(fmt::format("Genesis time is in the future. Will sleep for {} seconds",
      std::chrono::duration_cast<std::chrono::seconds>(initial_sleep_duration).count()));
    std::this_thread::sleep_for(initial_sleep_duration);
  }
  bs_reactor->on_start();

  cs_reactor->on_start();

  // TODO : start mempool_reactor

  ev_reactor->on_start();
}

void node::on_stop() {
  ev_reactor->on_stop();
  cs_reactor->on_stop();
  bs_reactor->on_stop();
}

state node::load_state_from_db_or_genesis(
  const std::shared_ptr<noir::consensus::db_store>& state_store, const std::shared_ptr<genesis_doc>& gen_doc) {
  // 1. Attempt to load state form the database
  state state_{};
  if (state_store->load(state_)) {
    dlog("successfully loaded state from db");
  } else {
    dlog("unable to load state from db");
    // return state_; // todo - what to do here?
  }

  if (state_.is_empty()) {
    // 2. If it's not there, derive it from the genesis doc
    state_ = state::make_genesis_state(*gen_doc);
  }

  return state_;
}

} // namespace noir::consensus
