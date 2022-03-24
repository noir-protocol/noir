// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <noir/commands/commands.h>
#include <noir/rpc/jsonrpc.h>
#include <noir/rpc/rpc.h>
#include <eth/rpc/rpc.h>

using namespace noir::rpc;

namespace noir::commands {

CLI::App* start(CLI::App& root) {
  return root.add_subcommand("start", "Run the NOIR node")->final_callback([]() {
    auto home_dir = app.home_dir();

    if (!app.initialize<rpc::rpc, rpc::jsonrpc, eth::rpc::rpc>()) {
      throw CLI::Success();
    }
    app.startup();
    app.exec();
  });
}

} // namespace noir::commands
