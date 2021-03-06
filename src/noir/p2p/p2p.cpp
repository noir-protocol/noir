// This file is part of NOIR.
//
// Copyright (c) 2017-2021 block.one and its contributors.  All rights reserved.
// SPDX-License-Identifier: MIT
//
#include <noir/codec/protobuf.h>
#include <noir/common/thread_pool.h>
#include <noir/common/types/varint.h>
#include <noir/consensus/abci.h>
#include <noir/consensus/tx.h>
#include <noir/consensus/types/encoding_helper.h>
#include <noir/consensus/types/node_info.h>
#include <noir/net/detail/message_buffer.h>
#include <noir/p2p/conn/secret_connection.h>
#include <noir/p2p/p2p.h>
#include <noir/p2p/queued_buffer.h>
#include <noir/p2p/types.h>
#include <tendermint/crypto/keys.pb.h>
#include <tendermint/p2p/conn.pb.h>

#include <appbase/application.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cppcodec/base64_default_rfc4648.hpp>
#include <google/protobuf/wrappers.pb.h>

#include <atomic>
#include <shared_mutex>

namespace noir::p2p {

using std::vector;

using boost::multi_index_container;
using boost::asio::ip::address_v4;
using boost::asio::ip::host_name;
using boost::asio::ip::tcp;

class connection : public std::enable_shared_from_this<connection> {
public:
  explicit connection(std::string endpoint);
  connection();

  ~connection() {}

  bool start_session();

  bool socket_is_open() const {
    return socket_open.load();
  } // thread safe, atomic
  const std::string& peer_address() const {
    return peer_addr;
  } // thread safe, const

  void set_heartbeat_timeout(std::chrono::seconds sec) {
    std::chrono::system_clock::duration dur = sec;
    hb_timeout = dur.count();
  }

private:
  static const std::string unknown;

  void update_endpoints();

  std::atomic<bool> socket_open{false};

  const std::string peer_addr;

public:
  boost::asio::io_context::strand strand;
  std::shared_ptr<tcp::socket> socket; // only accessed through strand after construction

  net::detail::message_buffer<1024 * 1024> pending_message_buffer;
  net::detail::message_buffer<8192> decrypted_message_buffer;
  std::atomic<std::size_t> outstanding_read_bytes{0}; // accessed only from strand threads

  queued_buffer buffer_queue;

  std::atomic<bool> connecting{true};
  std::atomic<bool> syncing{false};

  std::atomic<uint16_t> consecutive_immediate_connection_close = 0;

  std::mutex response_expected_timer_mtx;
  boost::asio::steady_timer response_expected_timer;

  std::atomic<go_away_reason> no_retry{no_reason};

  mutable std::mutex conn_mtx; //< mtx for last_req .. local_endpoint_port
  Bytes conn_node_id;
  std::string remote_endpoint_ip;
  std::string remote_endpoint_port;
  std::string local_endpoint_ip;
  std::string local_endpoint_port;

  connection_status get_status() const;

  tstamp latest_msg_time{0};
  tstamp hb_timeout;

  bool connected();
  bool current();

  void close(bool reconnect = true, bool shutdown = false);

private:
  static void _close(connection* self, bool reconnect, bool shutdown); // for easy capture

public:
  bool resolve_and_connect();
  void connect(const std::shared_ptr<tcp::resolver>& resolver, tcp::resolver::results_type endpoints);

  void check_heartbeat(tstamp current_time);

  const std::string peer_name();

  void enqueue(const envelope& msg);
  void enqueue_buffer(const std::shared_ptr<std::vector<unsigned char>>& send_buffer,
    go_away_reason close_after_send,
    bool to_sync_queue = false);
  void flush_queues();

  void cancel_wait();

  void queue_write(const std::shared_ptr<std::vector<unsigned char>>& buff,
    std::function<void(boost::system::error_code, std::size_t)> callback,
    bool to_sync_queue = false);
  void do_queue_write();

  std::shared_ptr<secret_connection> secret_conn{};
  std::function<Result<void>(std::shared_ptr<Bytes>)> cb_current_task;
  void start_handshake();
  void read_a_message(std::function<void(std::shared_ptr<Bytes>)>);
  void read_a_secret_message();
  void shared_eph_pub_key(std::shared_ptr<Bytes>);
  Result<int> write_msg(const Bytes&, bool use_secret_conn = true);
  bool process_next_message();
  Result<void> task_authenticate(std::shared_ptr<Bytes>);
  Result<void> task_node_info(std::shared_ptr<Bytes>);
  Result<void> task_process_message(std::shared_ptr<Bytes>);
  void send_message(const ::tendermint::p2p::PacketPing&);
  void send_message(const ::tendermint::p2p::PacketPong&);
  void send_message(const ::tendermint::p2p::PacketMsg&);
  void send_message(const ::tendermint::p2p::Packet&);
};

using connection_ptr = std::shared_ptr<connection>;
using connection_wptr = std::weak_ptr<connection>;

const std::string connection::unknown = "<unknown>";

//------------------------------------------------------------------------
// p2p_impl
//------------------------------------------------------------------------
class p2p_impl : public std::enable_shared_from_this<p2p_impl> {
public:
  p2p_impl(appbase::application& app): app(app) {}

  appbase::application& app;

  std::unique_ptr<tcp::acceptor> acceptor;

  /**
   * Thread safe, only updated in plugin initialize
   *  @{
   */
  std::string p2p_address;
  std::string p2p_server_address;

  vector<std::string> supplied_peers;

  boost::asio::steady_timer::duration connector_period{0};
  boost::asio::steady_timer::duration txn_exp_period{0};
  boost::asio::steady_timer::duration resp_expected_period{0};
  std::chrono::seconds keepalive_interval{std::chrono::seconds{60}};
  std::chrono::seconds heartbeat_timeout{std::chrono::seconds{90}};

  int max_cleanup_time_ms = 0;
  uint32_t max_client_count = 0;
  uint32_t max_nodes_per_host = 1;

  consensus::node_info my_node_info;
  Bytes20 node_id;

  // External plugins
  consensus::abci* abci_plug{nullptr};

  // Channels
  plugin_interface::incoming::channels::cs_reactor_message_queue::channel_type& cs_reactor_mq_channel =
    app.get_channel<plugin_interface::incoming::channels::cs_reactor_message_queue>();
  plugin_interface::incoming::channels::bs_reactor_message_queue::channel_type& bs_reactor_mq_channel =
    app.get_channel<plugin_interface::incoming::channels::bs_reactor_message_queue>();
  plugin_interface::incoming::channels::es_reactor_message_queue::channel_type& es_reactor_mq_channel =
    app.get_channel<plugin_interface::incoming::channels::es_reactor_message_queue>();
  plugin_interface::incoming::channels::tp_reactor_message_queue::channel_type& tp_reactor_mq_channel =
    app.get_channel<plugin_interface::incoming::channels::tp_reactor_message_queue>();
  plugin_interface::channels::update_peer_status::channel_type& update_peer_status_channel =
    app.get_channel<plugin_interface::channels::update_peer_status>();

  plugin_interface::egress::channels::transmit_message_queue::channel_type::handle xmt_mq_subscription =
    app.get_channel<plugin_interface::egress::channels::transmit_message_queue>().subscribe(
      std::bind(&p2p_impl::transmit_message, this, std::placeholders::_1));

  // Methods
  plugin_interface::methods::send_error_to_peer::method_type::handle send_error_to_peer_provider =
    app.get_method<plugin_interface::methods::send_error_to_peer>().register_provider(
      [this](const std::string& peer_id, std::span<const char> msg) -> void {
        send_peer_error(peer_id, msg);
        disconnect(peer_id);
      });
  /** @} */

  mutable std::shared_mutex connections_mtx;
  std::set<connection_ptr> connections;

  std::mutex connector_check_timer_mtx;
  std::unique_ptr<boost::asio::steady_timer> connector_check_timer;
  int connector_checks_in_flight{0};

  std::mutex expire_timer_mtx;
  std::unique_ptr<boost::asio::steady_timer> expire_timer;

  std::mutex keepalive_timer_mtx;
  std::unique_ptr<boost::asio::steady_timer> keepalive_timer;

  std::atomic<bool> in_shutdown{false};

  uint16_t thread_pool_size = 2;
  std::optional<named_thread_pool> thread_pool;

public:
  void update_chain_info();
  void start_listen_loop();
  void start_conn_timer(boost::asio::steady_timer::duration du, std::weak_ptr<connection> from_connection);
  void start_monitors();
  void connection_monitor(std::weak_ptr<connection> from_connection, bool reschedule);
  void ticker();
  connection_ptr find_connection(const std::string& host) const; // must call with held mutex

  void transmit_message(const envelope_ptr& env);
  void send_peer_error(const std::string& peer_id, std::span<const char> msg);
  void disconnect(const std::string& peer_id);
};

static p2p_impl* my_impl;

template<typename Function>
void for_each_connection(Function f) {
  std::shared_lock<std::shared_mutex> g(my_impl->connections_mtx);
  for (auto& c : my_impl->connections) {
    if (!f(c))
      return;
  }
}

void p2p_impl::start_monitors() {
  {
    std::scoped_lock g(connector_check_timer_mtx);
    connector_check_timer.reset(new boost::asio::steady_timer(my_impl->thread_pool->get_executor()));
  }
  start_conn_timer(connector_period, std::weak_ptr<connection>());
}

void p2p_impl::start_conn_timer(boost::asio::steady_timer::duration du, std::weak_ptr<connection> from_connection) {
  if (in_shutdown)
    return;
  std::scoped_lock g(connector_check_timer_mtx);
  ++connector_checks_in_flight;
  connector_check_timer->expires_from_now(du);
  connector_check_timer->async_wait([my = shared_from_this(), from_connection](boost::system::error_code ec) {
    std::unique_lock<std::mutex> g(my->connector_check_timer_mtx);
    int num_in_flight = --my->connector_checks_in_flight;
    g.unlock();
    if (!ec) {
      my->connection_monitor(from_connection, num_in_flight == 0);
    } else {
      if (num_in_flight == 0) {
        if (my->in_shutdown)
          return;
        elog(fmt::format("Error from connection check monitor: {}", ec.message()));
        my->start_conn_timer(my->connector_period, std::weak_ptr<connection>());
      }
    }
  });
}

void p2p_impl::connection_monitor(std::weak_ptr<connection> from_connection, bool reschedule) {
  auto max_time = get_time();
  max_time +=
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(max_cleanup_time_ms)).count();
  auto from = from_connection.lock();
  std::unique_lock<std::shared_mutex> g(connections_mtx);
  auto it = (from ? connections.find(from) : connections.begin());
  if (it == connections.end())
    it = connections.begin();
  size_t num_rm = 0, num_clients = 0, num_peers = 0;
  while (it != connections.end()) {
    if (get_time() >= max_time) {
      connection_wptr wit = *it;
      g.unlock();
      dlog(fmt::format("Exiting connection monitor early, ran out of time: {}", max_time - get_time()));
      if (reschedule) {
        start_conn_timer(std::chrono::milliseconds(1), wit); // avoid exhausting
      }
      return;
    }
    (*it)->peer_address().empty() ? ++num_clients : ++num_peers;
    if (!(*it)->socket_is_open() && !(*it)->connecting) {
      if (!(*it)->peer_address().empty()) {
        if (!(*it)->resolve_and_connect()) {
          it = connections.erase(it);
          --num_peers;
          ++num_rm;
          continue;
        }
      } else {
        --num_clients;
        ++num_rm;
        it = connections.erase(it);
        continue;
      }
    }
    ++it;
  }
  g.unlock();
  if (num_clients > 0 || num_peers > 0)
    ilog(fmt::format("p2p client connections: {}/{}, peer connections: {}/{}", num_clients, max_client_count, num_peers,
      supplied_peers.size()));
  dlog(fmt::format("connection monitor, removed {} connections", num_rm));
  if (reschedule) {
    start_conn_timer(connector_period, std::weak_ptr<connection>());
  }
}

void p2p_impl::update_chain_info() {}

connection_ptr p2p_impl::find_connection(const std::string& host) const {
  for (const auto& c : connections)
    if (c->peer_address() == host)
      return c;
  return connection_ptr();
}

void p2p_impl::start_listen_loop() {
  connection_ptr new_connection = std::make_shared<connection>();
  new_connection->connecting = true;
  new_connection->strand.post([this, new_connection = std::move(new_connection)]() {
    acceptor->async_accept(*new_connection->socket,
      boost::asio::bind_executor(
        new_connection->strand, [new_connection, socket = new_connection->socket, this](boost::system::error_code ec) {
          if (!ec) {
            uint32_t visitors = 0;
            uint32_t from_addr = 0;
            boost::system::error_code rec;
            const auto& paddr_add = socket->remote_endpoint(rec).address();
            std::string paddr_str;
            if (rec) {
              elog(fmt::format("Error getting remote endpoint: {}", rec.message()));
            } else {
              paddr_str = paddr_add.to_string();
              for_each_connection([&visitors, &from_addr, &paddr_str](auto& conn) {
                if (conn->socket_is_open()) {
                  if (conn->peer_address().empty()) {
                    ++visitors;
                    std::scoped_lock g_conn(conn->conn_mtx);
                    if (paddr_str == conn->remote_endpoint_ip) {
                      ++from_addr;
                    }
                  }
                }
                return true;
              });
              if (from_addr < max_nodes_per_host && (max_client_count == 0 || visitors < max_client_count)) {
                ilog(fmt::format("Accepted new connection: {}", paddr_str));
                new_connection->set_heartbeat_timeout(heartbeat_timeout);
                if (new_connection->start_session()) {
                  std::scoped_lock<std::shared_mutex> g_unique(connections_mtx);
                  connections.insert(new_connection);
                }

              } else {
                if (from_addr >= max_nodes_per_host) {
                  dlog(fmt::format("Number of connections ({}) from {} exceeds limit {}", from_addr + 1, paddr_str,
                    max_nodes_per_host));
                } else {
                  dlog(fmt::format("max_client_count {} exceeded", max_client_count));
                }
                // new_connection never added to connections and start_session not called, lifetime will end
                boost::system::error_code ec;
                socket->shutdown(tcp::socket::shutdown_both, ec);
                socket->close(ec);
              }
            }
          } else {
            elog(fmt::format("Error accepting connection: {}", ec.message()));
            // For the listed error codes below, recall start_listen_loop()
            switch (ec.value()) {
            case ECONNABORTED:
            case EMFILE:
            case ENFILE:
            case ENOBUFS:
            case ENOMEM:
            case EPROTO:
              break;
            default:
              return;
            }
          }
          start_listen_loop();
        }));
  });
}

void p2p_impl::ticker() {
  if (in_shutdown)
    return;
  std::scoped_lock g(keepalive_timer_mtx);
  keepalive_timer->expires_from_now(keepalive_interval);
  keepalive_timer->async_wait([my = shared_from_this()](boost::system::error_code ec) {
    my->ticker();
    if (ec) {
      if (my->in_shutdown)
        return;
      wlog(fmt::format("Peer keepalive ticked sooner than expected: {}", ec.message()));
    }

    tstamp current_time = get_time();
    for_each_connection([current_time](auto& c) {
      if (c->socket_is_open()) {
        c->strand.post([c, current_time]() { c->check_heartbeat(current_time); });
      }
      return true;
    });
  });
}

void p2p_impl::transmit_message(const envelope_ptr& env) {
  if (env->broadcast) {
    for_each_connection([env](auto& c) {
      if (c->socket_is_open() && c->conn_node_id.size() > 0) {
        c->strand.post([c, env]() { c->enqueue(*env); });
      }
      return true;
    });
  } else {
    // Unicast
    for_each_connection([env](auto& c) {
      if (c->socket_is_open() && (to_hex(c->conn_node_id) == env->to)) {
        dlog(fmt::format("unicast to={} size={}", env->to, env->message.size()));
        c->strand.post([c, env]() { c->enqueue(*env); });
        return false;
      }
      return true;
    });
  }
}

void p2p_impl::send_peer_error(const std::string& peer_id, std::span<const char> msg) {
  for_each_connection([peer_id, msg](auto& c) {
    if (c->socket_is_open() && (to_hex(c->conn_node_id) == peer_id)) {
      std::string str_msg(msg.begin(), msg.end());
      dlog(fmt::format("send peer_error to={} msg={}", peer_id, str_msg));
      envelope_ptr env = std::make_shared<envelope>();
      env->from = "";
      env->to = peer_id;
      env->broadcast = false;
      env->id = PeerError;
      env->message = Bytes(str_msg.begin(), str_msg.end());
      c->strand.post([c, env]() { c->enqueue(*env); });
      return false;
    }
    return true;
  });
}

void p2p_impl::disconnect(const std::string& peer_id) {
  for_each_connection([peer_id](auto& c) {
    if (c->socket_is_open() && (to_hex(c->conn_node_id) == peer_id)) {
      c->close(false);
      return false;
    }
    return true;
  });
}

//------------------------------------------------------------------------
// p2p
//------------------------------------------------------------------------
p2p::p2p(appbase::application& app): plugin(app), my(new p2p_impl(app)) {
  my_impl = my.get();
}

p2p::~p2p() {}

void p2p::set_program_options(CLI::App& config) {
  auto p2p_options = config.add_section("p2p",
    "###############################################\n"
    "###        P2P Configuration Options        ###\n"
    "###############################################");

  p2p_options
    ->add_option(
      "--p2p-listen-endpoint", my->p2p_address, "The actual host:port used to listen for incoming p2p connections.")
    ->force_callback()
    ->default_str("0.0.0.0:9876");
  p2p_options->add_option("--p2p-peer-address", my->supplied_peers, "The public endpoint of a peer node to connect to.")
    ->take_all();
}

void p2p::plugin_initialize(const CLI::App& config) {
  ilog("Initialize p2p");
  auto p2p_options = config.get_subcommand("p2p");

  my->connector_period = std::chrono::seconds(60); // number of seconds to wait before cleaning up dead connections
  my->max_cleanup_time_ms = 1000; // max connection cleanup time per cleanup call in millisec
  my->txn_exp_period = def_txn_expire_wait;
  my->resp_expected_period = def_resp_expected_wait;
  my->max_client_count = 5; // maximum number of clients from which connections are accepted, use 0 for no limit
  my->max_nodes_per_host = 1;
  my->keepalive_interval = std::chrono::seconds(60);
  my->heartbeat_timeout = std::chrono::seconds(90);

  // my->p2p_server_address = "0.0.0.0:9876"; // An externally accessible host:port for identifying this node.
  // Defaults to p2p-listen-endpoint
  my->thread_pool_size = 2; // number of threads to use

  // setup node_info
  auto abci_options = config.get_subcommand("abci");
  my->my_node_info.protocol_version.p2p = 8;
  my->my_node_info.protocol_version.block = 11;
  my->my_node_info.protocol_version.app = 0;
  my->my_node_info.listen_addr = "tcp://" + my->p2p_address;
  my->my_node_info.version = "0.35.6";
  my->my_node_info.channels = Bytes("402021222330386061626300");
  my->my_node_info.moniker = abci_options->get_option("--moniker")->as<std::string>();
  my->my_node_info.other.tx_index = "on";
  my->my_node_info.other.rpc_address = "tcp://0.0.0.0:26657"; // FIXME : properly use other node_info
}

void p2p::plugin_startup() {
  ilog("Start p2p");
  try {
    if (auto plug = app.find_plugin<consensus::abci>(); plug->get_state() == started) {
      ilog("abci_plugin is up and running; p2p <--> abci");
      my->abci_plug = plug;
      auto node_id = from_hex(my->abci_plug->node_->node_key_->node_id);
      std::copy(node_id.begin(), node_id.end(), my->node_id.begin());

      // finish setting up my node_info
      my->my_node_info.network = my->abci_plug->node_->genesis_doc_->chain_id;
      my->my_node_info.node_id.id = my->abci_plug->node_->node_key_->node_id;
    } else {
      ilog("abci_plugin is not running; will be simply testing p2p activities");
      crypto::rand_bytes({my->node_id.data(), my->node_id.size()});
    }
    ilog(fmt::format("my node_id is {}", my->node_id.to_string()));

    my->thread_pool.emplace("p2p", my->thread_pool_size);

    tcp::endpoint listen_endpoint;
    if (my->p2p_address.size() > 0) {
      auto host = my->p2p_address.substr(0, my->p2p_address.find(':'));
      auto port = my->p2p_address.substr(host.size() + 1, my->p2p_address.size());
      tcp::resolver resolver(my->thread_pool->get_executor());
      // Note: need to add support for IPv6 too?
      listen_endpoint = *resolver.resolve(tcp::v4(), host, port);

      my->acceptor.reset(new tcp::acceptor(my_impl->thread_pool->get_executor()));

      if (!my->p2p_server_address.empty()) {
        my->p2p_address = my->p2p_server_address;
      } else {
        if (listen_endpoint.address().to_v4() == address_v4::any()) {
          boost::system::error_code ec;
          auto host = host_name(ec);
          if (ec.value() != boost::system::errc::success) {
            throw Error(fmt::format("Unable to retrieve host_name. {}", ec.message()));
          }
          auto port = my->p2p_address.substr(my->p2p_address.find(':'), my->p2p_address.size());
          my->p2p_address = host + port;
        }
      }
    }

    if (my->acceptor) {
      try {
        my->acceptor->open(listen_endpoint.protocol());
        my->acceptor->set_option(tcp::acceptor::reuse_address(true));
        my->acceptor->bind(listen_endpoint);
        my->acceptor->listen();
      } catch (const std::exception& e) {
        elog(fmt::format("p2p::plugin_startup failed to bind to port {}", listen_endpoint.port()));
        throw e;
      }
      ilog(fmt::format("starting listener, max clients is {}", my->max_client_count));
      my->start_listen_loop();
    }

    {
      std::scoped_lock g(my->keepalive_timer_mtx);
      my->keepalive_timer.reset(new boost::asio::steady_timer(my->thread_pool->get_executor()));
    }
    my->ticker();

    my->start_monitors();

    my->update_chain_info();

    for (const auto& seed_node : my->supplied_peers) {
      if (!seed_node.empty())
        connect(seed_node);
    }

  } catch (...) {
    // always want plugin_shutdown even on exception
    plugin_shutdown();
    throw;
  }
}

void p2p::plugin_shutdown() {
  ilog("shutting down p2p");
  my->in_shutdown = true;
  for_each_connection([](auto& c) {
    c->close(false);
    return true;
  });
  my->keepalive_timer->cancel();
  // my->expire_timer->cancel(); // expire_timer is not initialized
  my->connector_check_timer->cancel();
  my->thread_pool->stop();
  my->thread_pool.reset();
}

std::string p2p::connect(const std::string& host) {
  std::scoped_lock<std::shared_mutex> g(my->connections_mtx);
  if (my->find_connection(host))
    return "already connected";

  connection_ptr c = std::make_shared<connection>(host);
  dlog(fmt::format("calling active connector: {}", host));
  if (c->resolve_and_connect()) {
    dlog(fmt::format("adding new connection to the list: {}", c->peer_name()));
    c->set_heartbeat_timeout(my->heartbeat_timeout);
    my->connections.insert(c);
  }
  return "added connection";
}

std::string p2p::disconnect(const std::string& host) {
  std::scoped_lock<std::shared_mutex> g(my->connections_mtx);
  for (auto itr = my->connections.begin(); itr != my->connections.end(); ++itr) {
    if ((*itr)->peer_address() == host) {
      ilog(fmt::format("disconnecting: {}", (*itr)->peer_name()));
      (*itr)->close();
      my->connections.erase(itr);
      return "connection removed";
    }
  }
  return "no known connection for host";
}

std::optional<connection_status> p2p::status(const std::string& endpoint) const {
  return std::optional<connection_status>();
}

std::vector<connection_status> p2p::connections() const {
  vector<connection_status> result;
  std::shared_lock<std::shared_mutex> g(my->connections_mtx);
  result.reserve(my->connections.size());
  for (const auto& c : my->connections) {
    result.push_back(c->get_status());
  }
  return result;
}

//------------------------------------------------------------------------
// connection
//------------------------------------------------------------------------
connection::connection(std::string endpoint)
  : peer_addr(endpoint),
    strand(my_impl->thread_pool->get_executor()),
    socket(new tcp::socket(my_impl->thread_pool->get_executor())),
    response_expected_timer(my_impl->thread_pool->get_executor()) {
  ilog(fmt::format("creating connection to {}", endpoint));
  latest_msg_time =
    get_time() + std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(20)).count();
}

connection::connection()
  : peer_addr(),
    strand(my_impl->thread_pool->get_executor()),
    socket(new tcp::socket(my_impl->thread_pool->get_executor())),
    response_expected_timer(my_impl->thread_pool->get_executor()) {
  dlog("new connection object created");
  latest_msg_time =
    get_time() + std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(20)).count();
}

bool connection::resolve_and_connect() {
  switch (no_retry) {
  case no_reason:
  case benign_other:
    break;
  default:
    dlog(fmt::format("Skipping connect due to go_away reason {}", reason_str(no_retry)));
    return false;
  }

  std::string::size_type colon = peer_address().find(':');
  if (colon == std::string::npos || colon == 0) {
    elog(fmt::format("Invalid peer address. must be \"host:port[:<blk>|<trx>]\": {}", peer_address()));
    return false;
  }

  connection_ptr c = shared_from_this();

  strand.post([c]() {
    std::string::size_type colon = c->peer_address().find(':');
    std::string::size_type colon2 = c->peer_address().find(':', colon + 1);
    std::string host = c->peer_address().substr(0, colon);
    std::string port =
      c->peer_address().substr(colon + 1, colon2 == std::string::npos ? std::string::npos : colon2 - (colon + 1));

    auto resolver = std::make_shared<tcp::resolver>(my_impl->thread_pool->get_executor());
    connection_wptr weak_conn = c;
    // Note: need to add support for IPv6 too
    resolver->async_resolve(tcp::v4(), host, port,
      boost::asio::bind_executor(
        c->strand, [resolver, weak_conn](const boost::system::error_code& err, tcp::resolver::results_type endpoints) {
          auto c = weak_conn.lock();
          if (!c)
            return;
          if (!err) {
            c->connect(resolver, endpoints);
          } else {
            elog(fmt::format("Unable to resolve {}: {}", c->peer_name(), err.message()));
            c->connecting = false;
            ++c->consecutive_immediate_connection_close;
          }
        }));
  });
  return true;
}

connection_status connection::get_status() const {
  connection_status stat;
  stat.peer = peer_addr;
  stat.connecting = connecting;
  stat.syncing = syncing;
  std::scoped_lock g(conn_mtx);
  return stat;
}

bool connection::connected() {
  return socket_is_open() && !connecting;
}

void connection::connect(const std::shared_ptr<tcp::resolver>& resolver, tcp::resolver::results_type endpoints) {
  switch (no_retry) {
  case no_reason:
  case benign_other:
    break;
  default:
    return;
  }
  connecting = true;
  pending_message_buffer.reset();
  decrypted_message_buffer.reset();
  buffer_queue.clear_out_queue();
  boost::asio::async_connect(*socket, endpoints,
    boost::asio::bind_executor(strand,
      [resolver, c = shared_from_this(), socket = socket](
        const boost::system::error_code& err, const tcp::endpoint& endpoint) {
        if (!err && socket->is_open() && socket == c->socket) {
          c->start_session();
        } else {
          elog(fmt::format("connection failed to {}: {}", c->peer_name(), err.message()));
          c->close(false);
        }
      }));
}

bool connection::start_session() {
  update_endpoints();
  boost::asio::ip::tcp::no_delay nodelay(true);
  boost::system::error_code ec;
  socket->set_option(nodelay, ec);
  if (ec) {
    elog(fmt::format("connection failed (set_option) {}: {}", peer_name(), ec.message()));
    close();
    return false;
  } else {
    dlog(fmt::format("connected to {}", peer_name()));
    socket_open = true;
    start_handshake();
    return true;
  }
}

void connection::close(bool reconnect, bool shutdown) {
  strand.post(
    [self = shared_from_this(), reconnect, shutdown]() { connection::_close(self.get(), reconnect, shutdown); });
}

void connection::_close(connection* self, bool reconnect, bool shutdown) {
  self->socket_open = false;
  boost::system::error_code ec;
  if (self->socket->is_open()) {
    self->socket->shutdown(tcp::socket::shutdown_both, ec);
    self->socket->close(ec);
  }
  self->socket.reset(new tcp::socket(my_impl->thread_pool->get_executor()));
  self->flush_queues();
  self->connecting = false;
  self->syncing = false;
  ++self->consecutive_immediate_connection_close;
  bool has_last_req = false;
  {
    std::scoped_lock g_conn(self->conn_mtx);
    self->conn_node_id = Bytes();
  }
  ilog(fmt::format("closing '{}', {}", self->peer_address(), self->peer_name()));
  dlog(fmt::format("canceling wait on {}", self->peer_name())); // peer_name(), do not hold conn_mtx
  self->cancel_wait();

  if (reconnect && !shutdown) {
    my_impl->start_conn_timer(std::chrono::milliseconds(100), connection_wptr());
  }
}

const std::string connection::peer_name() {
  std::scoped_lock g_conn(conn_mtx);
  if (!peer_address().empty()) {
    return peer_address();
  }
  if (remote_endpoint_port != unknown) {
    return remote_endpoint_ip + ":" + remote_endpoint_port;
  }
  return "connecting client";
}

void connection::update_endpoints() {
  boost::system::error_code ec;
  boost::system::error_code ec2;
  auto rep = socket->remote_endpoint(ec);
  auto lep = socket->local_endpoint(ec2);
  std::scoped_lock g_conn(conn_mtx);
  remote_endpoint_ip = ec ? unknown : rep.address().to_string();
  remote_endpoint_port = ec ? unknown : std::to_string(rep.port());
  local_endpoint_ip = ec2 ? unknown : lep.address().to_string();
  local_endpoint_port = ec2 ? unknown : std::to_string(lep.port());
}

void connection::cancel_wait() {
  std::scoped_lock g(response_expected_timer_mtx);
  response_expected_timer.cancel();
}

void connection::flush_queues() {
  buffer_queue.clear_write_queue();
}

void connection::enqueue(const envelope& m) {
  ::tendermint::p2p::PacketMsg msg;
  msg.set_channel_id(m.id);
  msg.set_data({m.message.begin(), m.message.end()});
  msg.set_eof(true);
  send_message(msg);
}

void connection::enqueue_buffer(
  const std::shared_ptr<std::vector<unsigned char>>& send_buffer, go_away_reason close_after_send, bool to_sync_queue) {
  connection_ptr self = shared_from_this();
  queue_write(
    send_buffer,
    [conn{std::move(self)}, close_after_send](boost::system::error_code ec, std::size_t) {
      if (ec)
        return;
      if (close_after_send != no_reason) {
        ilog(fmt::format(
          "sent a go away message: {}, closing connection to {}", reason_str(close_after_send), conn->peer_name()));
        conn->close();
        return;
      }
    },
    to_sync_queue);
}

void connection::queue_write(const std::shared_ptr<vector<unsigned char>>& buff,
  std::function<void(boost::system::error_code, std::size_t)> callback,
  bool to_sync_queue) {
  if (!buffer_queue.add_write_queue(buff, callback, to_sync_queue)) {
    wlog(fmt::format(
      "write_queue full {} bytes, giving up on connection {}", buffer_queue.write_queue_size(), peer_name()));
    close();
    return;
  }
  do_queue_write();
}

void connection::do_queue_write() {
  if (!buffer_queue.ready_to_send())
    return;
  connection_ptr c(shared_from_this());

  std::vector<boost::asio::const_buffer> bufs;
  buffer_queue.fill_out_buffer(bufs);

  strand.post([c{std::move(c)}, bufs{std::move(bufs)}]() {
    boost::asio::async_write(*c->socket, bufs,
      boost::asio::bind_executor(c->strand, [c, socket = c->socket](boost::system::error_code ec, std::size_t w) {
        try {
          c->buffer_queue.clear_out_queue();
          // May have closed connection and cleared buffer_queue
          if (!c->socket_is_open() || socket != c->socket) {
            ilog(fmt::format(
              "async write socket {} before callback: {}", c->socket_is_open() ? "changed" : "closed", c->peer_name()));
            c->close();
            return;
          }

          if (ec) {
            if (ec.value() != boost::asio::error::eof) {
              elog(fmt::format("Error sending to peer {}: {}", c->peer_name(), ec.message()));
            } else {
              wlog(fmt::format("connection closure detected on write to {}", c->peer_name()));
            }
            c->close();
            return;
          }

          c->buffer_queue.out_callback(ec, w);

          c->do_queue_write();
        } catch (const std::bad_alloc&) {
          throw;
        } catch (const boost::interprocess::bad_alloc&) {
          throw;
        } catch (const std::exception& ex) {
          elog(fmt::format("Exception in do_queue_write to {} {}", c->peer_name(), ex.what()));
        } catch (...) {
          elog(fmt::format("Exception in do_queue_write to {}", c->peer_name()));
        }
      }));
  });
}

void connection::check_heartbeat(tstamp current_time) {
  if (latest_msg_time > 0 && current_time > latest_msg_time + hb_timeout) {
    no_retry = benign_other;
    if (!peer_address().empty()) {
      wlog(fmt::format("heartbeat timed out for peer address {}", peer_address()));
      close(true); // reconnect
    } else {
      {
        std::scoped_lock g_conn(conn_mtx);
        wlog("heartbeat timed out from peer ");
      }
      close(false); // don't reconnect
    }
    return;
  }
  send_message(::tendermint::p2p::PacketPing{});
}

void connection::start_handshake() {
  // TODO : start timeout for handshake, which defaults to 20s (configurable)
  // Generated key must conform to Ed25519 Validation Rules by ZIP-215
  Bytes loc_priv_key = my_impl->abci_plug->node_->node_key_->priv_key;
  secret_conn = secret_connection::make_secret_connection(loc_priv_key);

  // Exchange loc_eph_pub
  Bytes bz(secret_conn->loc_eph_pub.size());
  std::memcpy(bz.data(), secret_conn->loc_eph_pub.data(), bz.size());
  auto my_msg = consensus::cdc_encode(bz);
  write_msg(my_msg, false); // send; use non-secret connection
  cb_current_task = [conn = shared_from_this()](
                      std::shared_ptr<Bytes> msg) -> Result<void> { return conn->task_authenticate(msg); };
  read_a_message([conn = shared_from_this()](
                   std::shared_ptr<Bytes> msg) -> void { return conn->shared_eph_pub_key(msg); }); // receive
}

void connection::read_a_message(std::function<void(std::shared_ptr<Bytes>)> cb) {
  try {
    std::size_t minimum_read =
      std::atomic_exchange<decltype(outstanding_read_bytes.load())>(&outstanding_read_bytes, 0);
    minimum_read = minimum_read != 0 ? minimum_read : 1;
    auto completion_handler = [minimum_read](
                                boost::system::error_code ec, std::size_t bytes_transferred) -> std::size_t {
      if (ec || bytes_transferred >= minimum_read)
        return 0;
      return minimum_read - bytes_transferred;
    };
    boost::asio::async_read(*socket, pending_message_buffer.get_buffer_sequence_for_boost_async_read().value(),
      completion_handler,
      boost::asio::bind_executor(strand,
        [conn = shared_from_this(), socket = socket, cb](boost::system::error_code ec, std::size_t bytes_transferred) {
          if (!conn->socket_is_open() || socket != conn->socket)
            return;
          if (!ec) {
            conn->pending_message_buffer.advance_write_ptr(bytes_transferred);
            while (conn->pending_message_buffer.bytes_to_read() > 0) {
              uint32_t bytes_in_buffer = conn->pending_message_buffer.bytes_to_read();
              try {
                varuint64 message_length = 0;
                net::detail::mb_peek_datastream ds(conn->pending_message_buffer);
                auto message_header_bytes = read_uleb128(ds, message_length);
                auto total_message_bytes = message_length + message_header_bytes;
                if (bytes_in_buffer >= total_message_bytes) {
                  conn->pending_message_buffer.advance_read_ptr(message_header_bytes);
                  conn->consecutive_immediate_connection_close = 0;
                  auto new_message = std::make_shared<Bytes>(message_length);
                  std::memcpy(new_message->data(), conn->pending_message_buffer.read_ptr(), message_length);
                  conn->pending_message_buffer.advance_read_ptr(message_length);
                  cb(new_message);
                  return;
                } else {
                  auto outstanding_message_bytes = total_message_bytes - bytes_in_buffer;
                  auto available_buffer_bytes = conn->pending_message_buffer.bytes_to_write();
                  if (outstanding_message_bytes > available_buffer_bytes)
                    conn->pending_message_buffer.add_space(outstanding_message_bytes - available_buffer_bytes);
                  conn->outstanding_read_bytes = outstanding_message_bytes;
                  break;
                }
              } catch (std::out_of_range& e) {
                conn->outstanding_read_bytes = 1;
              }
            }
            conn->read_a_message([conn, cb](std::shared_ptr<Bytes> msg) -> void { return cb(msg); });
          }
        }));
  } catch (...) {
    close();
  }
}

void connection::read_a_secret_message() {
  try {
    std::size_t minimum_read =
      std::atomic_exchange<decltype(outstanding_read_bytes.load())>(&outstanding_read_bytes, 0);
    minimum_read = minimum_read != 0 ? minimum_read : sealed_frame_size;

    auto completion_handler = [minimum_read](
                                boost::system::error_code ec, std::size_t bytes_transferred) -> std::size_t {
      if (ec || bytes_transferred >= minimum_read)
        return 0;
      else
        return minimum_read - bytes_transferred;
    };

    uint32_t write_queue_size = buffer_queue.write_queue_size();
    if (write_queue_size > def_max_write_queue_size) {
      elog(fmt::format("write queue full {} bytes, giving up on connection, closing connection to: {}",
        write_queue_size, peer_name()));
      close(false);
      return;
    }

    boost::asio::async_read(*socket, pending_message_buffer.get_buffer_sequence_for_boost_async_read().value(),
      completion_handler,
      boost::asio::bind_executor(strand,
        [conn = shared_from_this(), socket = socket](boost::system::error_code ec, std::size_t bytes_transferred) {
          // may have closed connection and cleared pending_message_buffer
          if (!conn->socket_is_open() || socket != conn->socket)
            return;
          bool close_connection{false};
          try {
            if (!ec) {
              if (bytes_transferred > conn->pending_message_buffer.bytes_to_write()) {
                elog(fmt::format("async_read_some callback: bytes_transferred = {}, buffer.bytes_to_write = {}",
                  bytes_transferred, conn->pending_message_buffer.bytes_to_write()));
              }
              conn->pending_message_buffer.advance_write_ptr(bytes_transferred);
              while (conn->pending_message_buffer.bytes_to_read() > 0) {
                uint32_t bytes_in_buffer = conn->pending_message_buffer.bytes_to_read();

                if (bytes_in_buffer < sealed_frame_size) {
                  conn->outstanding_read_bytes = sealed_frame_size - bytes_in_buffer;
                  break;
                } else {
                  if (auto ok = conn->secret_conn->read(std::span<unsigned char>(
                        reinterpret_cast<unsigned char*>(conn->pending_message_buffer.read_ptr()), sealed_frame_size));
                      (!ok)) {
                    elog("getting pending frame failed");
                    throw;
                  } else {
                    auto frame = ok.value();
                    std::copy(frame->begin(), frame->end(), conn->decrypted_message_buffer.write_ptr());
                    conn->decrypted_message_buffer.advance_write_ptr(frame->size());
                  }
                  conn->pending_message_buffer.advance_read_ptr(sealed_frame_size);
                  conn->latest_msg_time = get_time();

                  if (!conn->process_next_message())
                    conn->close();
                }
              }
              conn->read_a_secret_message();

            } else {
              if (ec.value() != boost::asio::error::eof)
                elog(fmt::format("Error reading message: {}", ec.message()));
              else
                ilog("Peer closed connection");
              close_connection = true;
            }
          } catch (const std::bad_alloc&) {
            throw;
          } catch (const boost::interprocess::bad_alloc&) {
            throw;
          } catch (const std::exception& ex) {
            elog(fmt::format("Exception in handling read data: {}", ex.what()));
            close_connection = true;
          } catch (...) {
            elog("Undefined exception handling read data");
            close_connection = true;
          }
          if (close_connection) {
            elog(fmt::format("Closing connection to: {}", conn->peer_name()));
            conn->close();
            ///< notify consensus of peer down
            my_impl->update_peer_status_channel.publish(appbase::priority::medium,
              std::make_shared<plugin_interface::peer_status_info>(
                plugin_interface::peer_status_info{to_hex(conn->conn_node_id), peer_status::down}));
          }
        }));
  } catch (...) {
    elog(fmt::format("Undefined exception in start_read_message, closing connection to: {}", peer_name()));
    close();
  }
}

void connection::shared_eph_pub_key(std::shared_ptr<Bytes> new_message) {
  dlog(fmt::format("shared_eph_pub_key = {}", to_hex(*new_message)));
  google::protobuf::BytesValue v;
  v.ParseFromArray(new_message->data(), new_message->size());
  Bytes32 received_eph_pub{v.value().begin(), v.value().end()};

  secret_conn->shared_eph_pub_key(received_eph_pub);

  // Exchange auth_sig_message : (1) loc_pub_key (2) loc_signature
  ::tendermint::crypto::PublicKey pb_key;
  *pb_key.mutable_ed25519() = {secret_conn->loc_pub_key.begin(), secret_conn->loc_pub_key.end()};
  ::tendermint::p2p::AuthSigMessage pb_auth;
  *pb_auth.mutable_pub_key() = pb_key;
  *pb_auth.mutable_sig() = {secret_conn->loc_signature.begin(), secret_conn->loc_signature.end()};
  auto bz = noir::codec::protobuf::encode(pb_auth);
  write_msg(bz); // send
  read_a_secret_message();
}

Result<int> connection::write_msg(const Bytes& bz, bool use_secret_conn) {
  go_away_reason close_after_send = no_reason;
  varint64 payload_size = bz.size();
  std::array<unsigned char, 10> t_buffer{};
  datastream<unsigned char> t_ds(t_buffer);
  auto header_size = write_uleb128(t_ds, payload_size);
  const size_t buffer_size = header_size + payload_size;
  auto send_buffer = std::make_shared<std::vector<unsigned char>>(buffer_size);

  datastream<unsigned char> ds(send_buffer->data(), buffer_size);
  write_uleb128(ds, payload_size);
  ds.write(bz.data(), payload_size);

  if (use_secret_conn) {
    // must use encrypted channel
    auto ok = secret_conn->write(std::span<unsigned char>(send_buffer->data(), send_buffer->size()));
    if (!ok)
      return Error::format("failed to convert message to encrypted ones");
    for (auto& msg : ok.value().second) {
      auto temp_buff = std::make_shared<std::vector<unsigned char>>(msg->size());
      std::memcpy(temp_buff->data(), msg->data(), msg->size());
      enqueue_buffer(temp_buff, close_after_send);
    }
    return ok.value().first;
  }

  enqueue_buffer(send_buffer, close_after_send);
  return send_buffer->size();
}

bool connection::process_next_message() {
  auto bytes_available = decrypted_message_buffer.bytes_to_read();
  if (bytes_available < 10)
    return true;
  varuint64 message_length = 0;
  net::detail::mb_peek_datastream ds(decrypted_message_buffer);
  auto message_header_bytes = read_uleb128(ds, message_length);
  if (bytes_available < message_header_bytes + message_length)
    return true;
  auto bz = std::make_shared<Bytes>(message_length);
  std::memcpy(bz->data(), decrypted_message_buffer.read_ptr() + message_header_bytes, message_length);
  decrypted_message_buffer.advance_read_ptr(message_header_bytes + message_length);
  if (auto ok = cb_current_task(bz); !ok) {
    elog(fmt::format("{}", ok.error().message()));
    return false;
  }
  return process_next_message();
}

Result<void> connection::task_authenticate(std::shared_ptr<Bytes> bz) {
  ::tendermint::p2p::AuthSigMessage pb;
  pb.ParseFromArray(bz->data(), bz->size());
  auth_sig_message m;
  m.key = pb.pub_key().ed25519();
  m.sig = pb.sig();
  secret_conn->shared_auth_sig(m);
  dlog(fmt::format("secret_conn: is_authorized={}", secret_conn->is_authorized));
  if (!secret_conn->is_authorized)
    return Error::format("failed to establish a secret_connection");

  cb_current_task = [conn = shared_from_this()](
                      std::shared_ptr<Bytes> msg) -> Result<void> { return conn->task_node_info(msg); };

  // Exchange node_info
  auto pb_my_node_info = consensus::node_info::to_proto(my_impl->my_node_info);
  auto bz_my_node_info = noir::codec::protobuf::encode(*pb_my_node_info);
  write_msg(bz_my_node_info); // send
  return success();
}

Result<void> connection::task_node_info(std::shared_ptr<Bytes> bz) {
  ::tendermint::p2p::NodeInfo pb;
  pb.ParseFromArray(bz->data(), bz->size());
  auto peer_info = consensus::node_info::from_proto(pb);
  ilog(fmt::format("node_info: peer={}", peer_info->node_id.id));
  conn_node_id = from_hex(peer_info->node_id.id);

  cb_current_task = [conn = shared_from_this()](
                      std::shared_ptr<Bytes> msg) -> Result<void> { return conn->task_process_message(msg); };

  ///< notify consensus of peer up
  my_impl->update_peer_status_channel.publish(appbase::priority::medium,
    std::make_shared<plugin_interface::peer_status_info>(
      plugin_interface::peer_status_info{to_hex(conn_node_id), peer_status::up}));
  return success();
}

Result<void> connection::task_process_message(std::shared_ptr<Bytes> bz) {
  dlog(fmt::format("process a message: size={}", bz->size()));
  auto pb_packet = noir::codec::protobuf::decode<::tendermint::p2p::Packet>(*bz);
  if (pb_packet.sum_case() == tendermint::p2p::Packet::kPacketPing) {
    dlog(" >> PING");
    send_message(::tendermint::p2p::PacketPong{});
    dlog(" << PONG");
  } else if (pb_packet.sum_case() == tendermint::p2p::Packet::kPacketPong) {
    dlog(" >> PONG");
  } else if (pb_packet.sum_case() == tendermint::p2p::Packet::kPacketMsg) {
    const auto& msg = pb_packet.packet_msg();
    dlog(fmt::format(" >> MSG : channel_id={} eof={} data={}", msg.channel_id(), msg.eof(), to_hex(msg.data())));
    auto new_envelope = std::make_shared<envelope>();
    new_envelope->from = to_hex(conn_node_id);
    new_envelope->id = static_cast<channel_id>(msg.channel_id());
    new_envelope->message = from_hex(to_hex(msg.data()));

    switch (new_envelope->id) {
    case State:
    case Data:
    case Vote:
    case VoteSetBits:
      // case Consensus:
      my_impl->cs_reactor_mq_channel.publish( ///< notify consensus reactor to take additional actions
        appbase::priority::medium, new_envelope);
      break;
    case BlockSync:
      my_impl->bs_reactor_mq_channel.publish( ///< notify block_sync reactor to take additional actions
        appbase::priority::medium, new_envelope);
      break;
    case Evidence:
      my_impl->es_reactor_mq_channel.publish( ///< notify evidence reactor to take additional actions
        appbase::priority::medium, new_envelope);
      break;
    case PeerError:
      elog(fmt::format("received peer_error from={} error={}", new_envelope->from, to_hex(msg.data())));
      my_impl->disconnect(new_envelope->from);
      break;
    default:
      wlog(fmt::format("unsupported channel_id={}", static_cast<int>(new_envelope->id)));
    }
  } else {
    ilog("UNKNOWN");
  }

  return success();
}

void connection::send_message(const ::tendermint::p2p::PacketPing& pp) {
  ::tendermint::p2p::Packet packet;
  auto ping = packet.mutable_packet_ping();
  *ping = pp;
  send_message(packet);
}
void connection::send_message(const ::tendermint::p2p::PacketPong& pp) {
  ::tendermint::p2p::Packet packet;
  auto pong = packet.mutable_packet_pong();
  *pong = pp;
  send_message(packet);
}
void connection::send_message(const ::tendermint::p2p::PacketMsg& pm) {
  ::tendermint::p2p::Packet packet;
  auto msg = packet.mutable_packet_msg();
  *msg = pm;
  send_message(packet);
}
void connection::send_message(const ::tendermint::p2p::Packet& packet) {
  auto bz = codec::protobuf::encode(packet);
  write_msg(bz);
}

} // namespace noir::p2p
