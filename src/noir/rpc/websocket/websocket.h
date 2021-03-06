// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <appbase/application.hpp>
#include <fc/variant.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <map>

namespace noir::rpc {

using message_sender = std::function<void(std::optional<fc::variant>)>;
using message_handler = std::function<void(std::string, message_sender)>;
using internal_message_handler =
  std::function<void(websocketpp::server<websocketpp::config::asio>::connection_ptr, std::string, message_sender)>;

using ws_server_type = websocketpp::server<websocketpp::config::asio>;

class websocket {
public:
  websocket(appbase::application& app): app(app){};

  void add_message_api(const std::string&, message_handler, int priority = appbase::priority::medium_low);
  void add_message_handler(const std::string&, message_handler&, int priority);
  static internal_message_handler make_app_thread_message_handler(
    appbase::application& app, message_handler, int priority);
  static message_sender make_message_sender(appbase::application& app,
    websocketpp::server<websocketpp::config::asio>::connection_ptr,
    int priority = appbase::priority::medium_low);
  void handle_message(
    websocketpp::server<websocketpp::config::asio>::connection_ptr conn, ws_server_type::message_ptr msg);

  std::map<std::string, internal_message_handler> message_handlers;

private:
  appbase::application& app;
};
} // namespace noir::rpc
