// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <tendermint/abci/client/client.h>

namespace tendermint::abci {

void ReqRes::set_callback(std::function<void(Response*)> cb) {
  std::unique_lock _(mtx);
  if (done) {
    _.unlock();
    cb(response.get());
    return;
  }
  this->cb = cb;
}

void ReqRes::invoke_callback() const {
  std::scoped_lock _(mtx);
  if (cb) {
    cb(response.get());
  }
}

std::function<void(Response*)> ReqRes::get_callback() const {
  std::scoped_lock _(mtx);
  return cb;
}

void ReqRes::set_done() {
  std::scoped_lock _(mtx);
  done = true;
}

void ReqRes::wait() const {
  while (!done) {
  }
}

} // namespace tendermint::abci
