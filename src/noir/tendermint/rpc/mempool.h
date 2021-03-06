// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/tendermint/rpc/responses.h>
#include <noir/tx_pool/tx_pool.h>

namespace noir::tendermint::rpc {

class mempool {
public:
  result_broadcast_tx broadcast_tx_async(const Bytes& tx);
  result_broadcast_tx broadcast_tx_sync(const Bytes& tx);
  //  result_broadcast_tx_commit broadcast_tx_commit(const tx& t);
  result_unconfirmed_txs unconfirmed_txs(const uint32_t& limit_ptr);
  result_unconfirmed_txs num_unconfirmed_txs();
  noir::consensus::response_check_tx& check_tx(const Bytes& tx);

  void set_tx_pool_ptr(noir::tx_pool::tx_pool* tx_pool_ptr) {
    this->tx_pool_ptr = tx_pool_ptr;
  }

private:
  noir::tx_pool::tx_pool* tx_pool_ptr;
};

} // namespace noir::tendermint::rpc
