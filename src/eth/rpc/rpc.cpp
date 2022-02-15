// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <noir/common/cli_helper.h>
#include <eth/rpc/api.h>
#include <eth/rpc/rpc.h>

namespace eth::rpc {

using namespace appbase;
using namespace noir;

rpc::rpc(): api(std::make_unique<api::api>()){};

void rpc::set_program_options(CLI::App& config) {
  auto eth_options = config.add_section("eth", "Ethereum Configuration");

  eth_options
    ->add_option("--rpc-tx-fee-cap",
      "RPC tx fee cap is the global transaction fee(price * gaslimit) cap for send transaction variants")
    ->default_val(0);
  eth_options->add_option("--rpc-allow-unprotected-txs", "Allow for unprotected transactions to be submitted via RPC")
    ->default_val(false);
}

void rpc::plugin_initialize(const CLI::App& config) {
  ilog("initializing ethereum rpc");

  auto eth_options = config.get_subcommand("eth");
  auto tx_fee_cap = eth_options->get_option("--rpc-tx-fee-cap")->as<uint256_t>();
  auto allow_unprotected_txs = eth_options->get_option("--rpc-allow-unprotected-txs")->as<bool>();

  api->set_tx_fee_cap(tx_fee_cap);
  api->set_allow_unprotected_txs(allow_unprotected_txs);
}

void rpc::plugin_startup() {
  ilog("starting ethereum rpc");

  auto& endpoint = app().get_plugin<noir::rpc::jsonrpc>().get_or_create_endpoint("/eth");
  endpoint.add_handler("eth_sendRawTransaction", [&](auto& req) { return api->send_raw_tx(req); });
}

void rpc::plugin_shutdown() {}

} // namespace eth::rpc
