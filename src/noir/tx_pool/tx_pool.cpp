// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <noir/core/codec.h>
#include <noir/tx_pool/tx_pool.h>
#include <algorithm>

namespace noir::tx_pool {

tx_pool::tx_pool(appbase::application& app)
  : plugin(app),
    config_(config{}),
    tx_queue_(config_.max_tx_num * config_.max_tx_bytes),
    tx_cache_(config_.max_tx_num),
    proxy_app_(std::make_shared<consensus::app_connection>()),
    xmt_mq_channel_(app.get_channel<plugin_interface::egress::channels::transmit_message_queue>()),
    msg_handle_(app.get_channel<plugin_interface::incoming::channels::tp_reactor_message_queue>().subscribe(
      [this](auto&& arg) { handle_msg(std::forward<decltype(arg)>(arg)); })) {}

tx_pool::tx_pool(appbase::application& app,
  const config& cfg,
  std::shared_ptr<consensus::app_connection>& new_proxy_app,
  uint64_t block_height)
  : plugin(app),
    config_(cfg),
    tx_queue_(config_.max_tx_num * config_.max_tx_bytes),
    tx_cache_(config_.max_tx_num),
    proxy_app_(new_proxy_app),
    block_height_(block_height),
    xmt_mq_channel_(app.get_channel<plugin_interface::egress::channels::transmit_message_queue>()),
    msg_handle_(app.get_channel<plugin_interface::incoming::channels::tp_reactor_message_queue>().subscribe(
      [this](auto&& arg) { handle_msg(std::forward<decltype(arg)>(arg)); })) {}

void tx_pool::set_program_options(CLI::App& cfg) {
  auto tx_pool_options = cfg.add_section("tx_pool",
    "###############################################\n"
    "###      TX_POOL Configuration Options      ###\n"
    "###############################################");

  tx_pool_options->add_option("--max_tx_num", "The maximum number of tx that the pool can store.")->default_val(10000);
  tx_pool_options->add_option("--max_tx_bytes", "The maximum bytes a single tx can hold.")->default_val(1024 * 1024);
  tx_pool_options->add_option("--ttl_duration", "Time(us) until tx expires in the pool. If it is '0', tx never expires")
    ->default_val(0);
  tx_pool_options
    ->add_option("--ttl_num_blocks", "Block height until tx expires in the pool. If it is '0', tx never expires")
    ->default_val(0);
  tx_pool_options->add_option("--gas_price_bump", "The minimum gas price for nonce override.")->default_val(1000);
}

void tx_pool::plugin_initialize(const CLI::App& config) {
  ilog("Initialize tx_pool");
  try {
    auto tx_pool_options = config.get_subcommand("tx_pool");

    config_.max_tx_num = tx_pool_options->get_option("--max_tx_num")->as<uint64_t>();
    config_.max_tx_bytes = tx_pool_options->get_option("--max_tx_bytes")->as<uint64_t>();
    config_.ttl_duration = tx_pool_options->get_option("--ttl_duration")->as<tstamp>();
    config_.ttl_num_blocks = tx_pool_options->get_option("--ttl_num_blocks")->as<uint64_t>();
    config_.gas_price_bump = tx_pool_options->get_option("--gas_price_bump")->as<uint64_t>();
  }
  FC_LOG_AND_RETHROW()
}

void tx_pool::plugin_startup() {
  ilog("Start tx_pool");
}

void tx_pool::plugin_shutdown() {
  ilog("Shutdown tx_pool");
}

void tx_pool::set_precheck(precheck_func* precheck) {
  precheck_ = precheck;
}

void tx_pool::set_postcheck(postcheck_func* postcheck) {
  postcheck_ = postcheck;
}

consensus::response_check_tx& tx_pool::check_tx_sync(const consensus::tx_ptr& tx_ptr) {
  auto tx_hash = consensus::get_tx_hash(*tx_ptr);
  check_tx_internal(tx_hash, tx_ptr);
  auto& res = proxy_app_->check_tx_sync(consensus::request_check_tx{.tx = *tx_ptr});
  add_tx(tx_hash, tx_ptr, res);
  return res;
}

void tx_pool::check_tx_async(const consensus::tx_ptr& tx_ptr) {
  auto tx_hash = consensus::get_tx_hash(*tx_ptr);
  check_tx_internal(tx_hash, tx_ptr);
  auto& req_res = proxy_app_->check_tx_async(consensus::request_check_tx{.tx = *tx_ptr});
  req_res.set_callback([&, tx_hash, tx_ptr](consensus::response_check_tx& res) { add_tx(tx_hash, tx_ptr, res); });
}

void tx_pool::check_tx_internal(const consensus::tx_hash& tx_hash, const consensus::tx_ptr& tx_ptr) {
  if (tx_ptr->size() > config_.max_tx_bytes) {
    FC_THROW_EXCEPTION(fc::tx_size_exception,
      fmt::format(
        "tx size {} bigger than {} (tx_hash: {})", tx_hash.to_string(), tx_ptr->size(), config_.max_tx_bytes));
  }

  if (precheck_ && !precheck_(*tx_ptr)) {
    FC_THROW_EXCEPTION(
      fc::bad_trasaction_exception, fmt::format("tx failed precheck (tx_hash: {})", tx_hash.to_string()));
  }

  tx_cache_.put(tx_hash, tx_ptr);
  if (tx_queue_.has(tx_hash)) {
    FC_THROW_EXCEPTION(
      fc::existed_tx_exception, fmt::format("tx already exists in pool (tx_hash: {})", tx_hash.to_string()));
  }
}

void tx_pool::add_tx(
  const consensus::tx_hash& tx_hash, const consensus::tx_ptr& tx_ptr, consensus::response_check_tx& res) {
  if (postcheck_ && !postcheck_(*tx_ptr, res)) {
    if (res.code != consensus::code_type_ok) {
      tx_cache_.del(tx_hash);
      FC_THROW_EXCEPTION(
        fc::bad_trasaction_exception, fmt::format("reject bad transaction (tx_hash: {})", tx_hash.to_string()));
    }
  }

  std::scoped_lock lock(mutex_);
  auto old = tx_queue_.get_tx(res.sender, res.nonce);
  if (old.has_value()) {
    auto& old_wtx = old.value();
    if (res.gas_wanted < old_wtx.gas + config_.gas_price_bump) {
      if (!config_.keep_invalid_txs_in_cache) {
        tx_cache_.del(tx_hash);
      }
      FC_THROW_EXCEPTION(fc::override_fail_exception,
        fmt::format(
          "gas price is not enough for nonce override (tx_hash: {}, nonce: {})", tx_hash.to_string(), res.nonce));
    }
    tx_queue_.erase(old_wtx.hash);
  }

  auto wtx = consensus::wrapped_tx(res.sender, tx_ptr, res.gas_wanted, res.nonce, block_height_);

  if (!tx_queue_.add_tx(wtx)) {
    if (!config_.keep_invalid_txs_in_cache) {
      tx_cache_.del(tx_hash);
    }
    FC_THROW_EXCEPTION(fc::full_pool_exception, fmt::format("Tx pool is full"));
  }

  if (config_.broadcast) {
    broadcast_tx(*tx_ptr);
  }
  dlog(fmt::format("tx_hash({}) is accepted.", tx_hash.to_string()));
}

std::vector<std::shared_ptr<const consensus::tx>> tx_pool::reap_max_bytes_max_gas(
  uint64_t max_bytes, uint64_t max_gas) {
  std::scoped_lock lock(mutex_);
  std::vector<std::shared_ptr<const consensus::tx>> txs;
  auto rbegin = tx_queue_.rbegin<unapplied_tx_queue::by_gas>(max_gas);
  auto rend = tx_queue_.rend<unapplied_tx_queue::by_gas>(0);
  txs.reserve(max_gas / rbegin->gas());
  uint64_t bytes = 0;
  uint64_t gas = 0;
  for (auto itr = rbegin; itr != rend; itr++) {
    auto& wtx = itr->wtx;
    auto& tx_ptr = wtx.tx_ptr;
    if (gas + wtx.gas > max_gas) {
      continue;
    }

    if (bytes + tx_ptr->size() > max_bytes) {
      break;
    }

    bytes += tx_ptr->size();
    gas += wtx.gas;
    txs.push_back(tx_ptr);
  }

  return txs;
}

std::vector<std::shared_ptr<const consensus::tx>> tx_pool::reap_max_txs(uint64_t tx_count) {
  std::scoped_lock lock(mutex_);
  uint64_t count = std::min<uint64_t>(tx_count, tx_queue_.size());

  std::vector<std::shared_ptr<const consensus::tx>> txs;
  txs.reserve(count);
  for (auto itr = tx_queue_.begin(); itr != tx_queue_.end(); itr++) {
    if (txs.size() >= count) {
      break;
    }
    txs.push_back(itr->wtx.tx_ptr);
  }

  return txs;
}

void tx_pool::update(uint64_t block_height,
  const std::vector<consensus::tx_ptr>& block_txs,
  std::vector<consensus::response_deliver_tx> responses,
  precheck_func* new_precheck,
  postcheck_func* new_postcheck) {
  std::scoped_lock lock(mutex_);
  block_height_ = block_height;

  if (new_precheck) {
    precheck_ = new_precheck;
  }

  if (new_postcheck) {
    postcheck_ = new_postcheck;
  }

  size_t size = std::min(block_txs.size(), responses.size());
  for (auto i = 0; i < size; i++) {
    auto tx_hash = consensus::get_tx_hash(*block_txs[i]);
    if (responses[i].code == consensus::code_type_ok) {
      tx_cache_.put(tx_hash, block_txs[i]);
    } else if (!config_.keep_invalid_txs_in_cache) {
      tx_cache_.del(tx_hash);
    }

    tx_queue_.erase(tx_hash);
  }

  if (config_.ttl_num_blocks > 0) {
    uint64_t expired_block_height = block_height_ > config_.ttl_num_blocks ? block_height_ - config_.ttl_num_blocks : 0;
    auto begin = tx_queue_.begin<unapplied_tx_queue::by_height>(0);
    auto end = tx_queue_.end<unapplied_tx_queue::by_height>(expired_block_height);
    for (auto& itr = begin; itr != end; itr++) {
      auto& wtx = itr->wtx;
      tx_queue_.erase(wtx.hash);
    }
  }

  if (config_.ttl_duration > 0) {
    auto expired_time = get_time() - config_.ttl_duration;
    auto begin = tx_queue_.begin<unapplied_tx_queue::by_time>(0);
    auto end = tx_queue_.end<unapplied_tx_queue::by_time>(expired_time);
    for (auto& itr = begin; itr != end; itr++) {
      auto& wtx = itr->wtx;
      tx_queue_.erase(wtx.hash);
    }
  }

  if (config_.recheck) {
    update_recheck_txs();
  }
}

void tx_pool::update_recheck_txs() {
  for (auto itr = tx_queue_.begin(); itr != tx_queue_.end(); itr++) {
    auto& wtx = itr->wtx;
    proxy_app_->check_tx_async(consensus::request_check_tx{
      .tx = *wtx.tx_ptr,
      .type = consensus::check_tx_type::recheck,
    });
    proxy_app_->flush_async();
  }
}

size_t tx_pool::size() const {
  return tx_queue_.size();
}

uint64_t tx_pool::size_bytes() const {
  return tx_queue_.bytes_size();
}

bool tx_pool::empty() const {
  return tx_queue_.empty();
}

void tx_pool::flush() {
  std::scoped_lock scoped_lock(mutex_);
  tx_queue_.clear();
  tx_cache_.reset();
}

void tx_pool::flush_app_conn() {
  proxy_app_->flush_sync();
}

void tx_pool::broadcast_tx(const consensus::tx& tx) {
  dlog(fmt::format("broadcast tx (tx_hash: {})", consensus::get_tx_hash(tx).to_string()));
  auto new_env = std::make_shared<p2p::envelope>();
  new_env->from = "";
  new_env->to = "";
  new_env->broadcast = true;
  new_env->id = p2p::Transaction;

  const uint32_t payload_size = encode_size(tx);
  new_env->message.resize(payload_size);
  datastream<unsigned char> ds(new_env->message.data(), payload_size);
  ds << tx;

  xmt_mq_channel_.publish(appbase::priority::medium, new_env);
}

void tx_pool::handle_msg(p2p::envelope_ptr msg) {
  datastream<unsigned char> ds(msg->message.data(), msg->message.size());
  consensus::tx tx;
  ds >> tx;

  check_tx_sync(std::make_shared<consensus::tx>(tx)); // TODO : sync only?
}

} // namespace noir::tx_pool
