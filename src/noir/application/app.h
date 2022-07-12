// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/consensus/abci_types.h>
#include <tendermint/abci/types.pb.h>

namespace noir::application {

using namespace tendermint::abci;

class base_application {
protected:
  // TODO : these variables are temporary. need to change to queue and need to be moved to another class.
  consensus::response_init_chain response_init_chain_;
  consensus::response_prepare_proposal response_prepare_proposal_;
  consensus::response_begin_block response_begin_block_;
  consensus::response_deliver_tx response_deliver_tx_;
  consensus::response_check_tx response_check_tx_;
  consensus::response_end_block response_end_block_;
  consensus::response_commit response_commit_;

  consensus::req_res<consensus::response_deliver_tx> req_res_deliver_tx_;
  consensus::req_res<consensus::response_check_tx> req_res_check_tx_;

public:
  base_application() {}
  // virtual void info() {}
  // virtual void query() {}

  virtual std::unique_ptr<ResponseInfo> info_sync(const RequestInfo& req) {
    return {};
  }

  virtual consensus::response_init_chain& init_chain() {
    return response_init_chain_;
  }
  virtual consensus::response_prepare_proposal& prepare_proposal() {
    return response_prepare_proposal_;
  }
  virtual consensus::response_begin_block& begin_block() {
    return response_begin_block_;
  }
  virtual consensus::req_res<consensus::response_deliver_tx>& deliver_tx_async() {
    return req_res_deliver_tx_;
  }
  virtual consensus::response_check_tx& check_tx_sync() {
    return response_check_tx_;
  }
  virtual consensus::req_res<consensus::response_check_tx>& check_tx_async() {
    return req_res_check_tx_;
  }
  virtual consensus::response_end_block& end_block() {
    return response_end_block_;
  }
  virtual consensus::response_commit& commit() {
    return response_commit_;
  }

  virtual void list_snapshots() {}
  virtual void offer_snapshot() {}
  virtual void load_snapshot_chunk() {}
  virtual void apply_snapshot_chunk() {}
};

} // namespace noir::application
