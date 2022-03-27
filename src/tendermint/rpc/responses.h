// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <noir/common/helper/variant.h>
#include <noir/common/refl.h>
#include <tendermint/abci/types.pb.h>
#include <tendermint/common/types.h>

namespace tendermint::rpc {

struct result_broadcast_tx {
  uint32_t code;
  bytes data;
  std::string log;
  std::string codespace;
  std::string mempool_error;

  bytes32 hash;
};

struct result_broadcast_tx_commit {
  abci::ResponseCheckTx check_tx;
  abci::ResponseDeliverTx deliver_tx;
  bytes hash;
  int64_t height;
};

struct result_unconfirmed_txs {
  uint64_t count;
  uint64_t total;
  uint64_t total_bytes;
  std::vector<tx> txs;
};

} // namespace tendermint::rpc

NOIR_REFLECT(tendermint::rpc::result_broadcast_tx, code, data, log, codespace, mempool_error, hash)
NOIR_REFLECT(tendermint::rpc::result_unconfirmed_txs, count, total, total_bytes, txs)