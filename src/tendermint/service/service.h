// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <tendermint/common/common.h>
#include <tendermint/log/log.h>
#include <atomic>

namespace tendermint::service {

const error err_already_started = "already started";
const error err_already_stopped = "already stopped";
const error err_not_started = "not started";

// XXX: optimize atomic operations in memory order - relaxed?
template<typename Derived>
class Service {
public:
  // result<void> on_start() noexcept;
  // void on_stop() noexcept;
  // result<void> on_reset() noexcept;

  result<void> start() noexcept {
    bool started_expected = false;
    if (started.compare_exchange_strong(started_expected, true)) {
      if (stopped.load()) {
        tm_elog(logger.get(), "Not starting {} service -- already stopped", name);
        started.store(false);
        return make_unexpected(err_already_stopped);
      }
      tm_ilog(logger.get(), "Starting {} service", name);
      auto res = static_cast<Derived*>(this)->on_start();
      if (!res) {
        started.store(false);
        return make_unexpected(res.error());
      }
      return {};
    }
    tm_dlog(logger.get(), "Not starting {} service -- already started", name);
    return make_unexpected(err_already_started);
  }

  result<void> stop() noexcept {
    bool stopped_expected = false;
    if (stopped.compare_exchange_strong(stopped_expected, true)) {
      if (!started.load()) {
        tm_elog(logger.get(), "Not stopping {} service -- has not been started yet", name);
        stopped.store(false);
        return make_unexpected(err_not_started);
      }
      tm_ilog(logger.get(), "Stopping {} service", name);
      static_cast<Derived*>(this)->on_stop();
      // quit
      return {};
    }
    tm_dlog(logger.get(), "Stopping {} service (already stopped)", name);
    return make_unexpected(err_already_stopped);
  }

  result<void> reset() noexcept {
    bool stopped_expected = true;
    if (!stopped.compare_exchange_strong(stopped_expected, false)) {
      tm_dlog(logger.get(), "Can't reset {} service, Not stopped", name);
      return errorf("can't reset running {}", name);
    }

    bool started_expected = true;
    started.compare_exchange_strong(started_expected, false);
    return static_cast<Derived*>(this)->on_reset();
  }

  bool is_running() const {
    return started.load() && !stopped.load();
  }

  const std::string& to_string() const {
    return name;
  }

  void set_logger(std::shared_ptr<log::logger> l) {
    this->logger = l;
  }

  // wait
  // quit

protected:
  std::string name;
  std::shared_ptr<log::logger> logger = log::default_logger();

private:
  std::atomic_bool started;
  std::atomic_bool stopped;
  // quit
};

} // namespace tendermint::service
