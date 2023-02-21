// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <seastar/core/shared_future.hh>
#include <seastar/core/sleep.hh>

#include "io_handler.h"

namespace crimson::net {

class ProtocolV2 final : public HandshakeListener {
  using AuthConnectionMetaRef = seastar::lw_shared_ptr<AuthConnectionMeta>;

public:
  ProtocolV2(SocketConnection &,
             IOHandler &);

  ~ProtocolV2() final;

  ProtocolV2(const ProtocolV2 &) = delete;
  ProtocolV2(ProtocolV2 &&) = delete;
  ProtocolV2 &operator=(const ProtocolV2 &) = delete;
  ProtocolV2 &operator=(ProtocolV2 &&) = delete;

/**
 * as HandshakeListener
 */
private:
  void notify_out() final;

  void notify_out_fault(const char *, std::exception_ptr) final;

  void notify_mark_down() final;

/*
* as ProtocolV2 to be called by SocketConnection
*/
public:
  void start_connect(const entity_addr_t& peer_addr,
                     const entity_name_t& peer_name);

  void start_accept(SocketRef&& socket,
                    const entity_addr_t& peer_addr);

  seastar::future<> close_clean_yielded();

#ifdef UNIT_TESTS_BUILT
  bool is_closed_clean() const {
    return closed_clean;
  }

  bool is_closed() const {
    return closed;
  }

#endif
private:
  seastar::future<> wait_exit_io() {
    if (exit_io.has_value()) {
      return exit_io->get_shared_future();
    } else {
      return seastar::now();
    }
  }

  enum class state_t {
    NONE = 0,
    ACCEPTING,
    SERVER_WAIT,
    ESTABLISHING,
    CONNECTING,
    READY,
    STANDBY,
    WAIT,
    REPLACING,
    CLOSING
  };

  static const char *get_state_name(state_t state) {
    const char *const statenames[] = {"NONE",
                                      "ACCEPTING",
                                      "SERVER_WAIT",
                                      "ESTABLISHING",
                                      "CONNECTING",
                                      "READY",
                                      "STANDBY",
                                      "WAIT",
                                      "REPLACING",
                                      "CLOSING"};
    return statenames[static_cast<int>(state)];
  }

  void trigger_state(state_t state, IOHandler::io_state_t io_state, bool reentrant);

  template <typename Func, typename T>
  void gated_execute(const char *what, T &who, Func &&func) {
    gate.dispatch_in_background(what, who, [this, &who, &func] {
      if (!execution_done.available()) {
        // discard the unready future
        gate.dispatch_in_background(
          "gated_execute_abandon",
          who,
          [fut=std::move(execution_done)]() mutable {
            return std::move(fut);
          }
        );
      }
      seastar::promise<> pr;
      execution_done = pr.get_future();
      return seastar::futurize_invoke(std::forward<Func>(func)
      ).finally([pr=std::move(pr)]() mutable {
        pr.set_value();
      });
    });
  }

  void fault(state_t expected_state,
             const char *where,
             std::exception_ptr eptr);

  void reset_session(bool is_full);
  seastar::future<std::tuple<entity_type_t, entity_addr_t>>
  banner_exchange(bool is_connect);

  enum class next_step_t {
    ready,
    wait,
    none,       // protocol should have been aborted or failed
  };

  // CONNECTING (client)
  seastar::future<> handle_auth_reply();
  inline seastar::future<> client_auth() {
    std::vector<uint32_t> empty;
    return client_auth(empty);
  }
  seastar::future<> client_auth(std::vector<uint32_t> &allowed_methods);

  seastar::future<next_step_t> process_wait();
  seastar::future<next_step_t> client_connect();
  seastar::future<next_step_t> client_reconnect();
  void execute_connecting();

  // ACCEPTING (server)
  seastar::future<> _auth_bad_method(int r);
  seastar::future<> _handle_auth_request(bufferlist& auth_payload, bool more);
  seastar::future<> server_auth();

  bool validate_peer_name(const entity_name_t& peer_name) const;
  seastar::future<next_step_t> send_wait();
  seastar::future<next_step_t> reuse_connection(ProtocolV2* existing_proto,
                                                bool do_reset=false,
                                                bool reconnect=false,
                                                uint64_t conn_seq=0,
                                                uint64_t msg_seq=0);

  seastar::future<next_step_t> handle_existing_connection(SocketConnectionRef existing_conn);
  seastar::future<next_step_t> server_connect();

  seastar::future<next_step_t> read_reconnect();
  seastar::future<next_step_t> send_retry(uint64_t connect_seq);
  seastar::future<next_step_t> send_retry_global(uint64_t global_seq);
  seastar::future<next_step_t> send_reset(bool full);
  seastar::future<next_step_t> server_reconnect();

  void execute_accepting();

  // CONNECTING/ACCEPTING
  seastar::future<> finish_auth();

  // ESTABLISHING
  void execute_establishing(SocketConnectionRef existing_conn);

  // ESTABLISHING/REPLACING (server)
  seastar::future<> send_server_ident();

  // REPLACING (server)
  void trigger_replacing(bool reconnect,
                         bool do_reset,
                         FrameAssemblerV2::mover_t &&mover,
                         AuthConnectionMetaRef&& new_auth_meta,
                         uint64_t new_peer_global_seq,
                         // !reconnect
                         uint64_t new_client_cookie,
                         entity_name_t new_peer_name,
                         uint64_t new_conn_features,
                         uint64_t new_peer_supported_features,
                         // reconnect
                         uint64_t new_connect_seq,
                         uint64_t new_msg_seq);

  // READY
  void execute_ready();

  // STANDBY
  void execute_standby();

  // WAIT
  void execute_wait(bool max_backoff);

  // SERVER_WAIT
  void execute_server_wait();

  // CLOSING
  // reentrant
  void do_close(bool is_dispatch_reset,
                std::optional<std::function<void()>> f_accept_new=std::nullopt);

private:
  SocketConnection &conn;

  SocketMessenger &messenger;

  IOHandler &io_handler;

  bool has_socket = false;

  // the socket exists and it is not shutdown
  bool is_socket_valid = false;

  FrameAssemblerV2Ref frame_assembler;

  std::optional<seastar::shared_promise<>> exit_io;

  AuthConnectionMetaRef auth_meta;

  crimson::common::Gated gate;

  bool closed = false;

  // become valid only after closed == true
  seastar::shared_future<> closed_clean_fut;

#ifdef UNIT_TESTS_BUILT
  bool closed_clean = false;

#endif
  state_t state = state_t::NONE;

  uint64_t peer_supported_features = 0;

  uint64_t client_cookie = 0;
  uint64_t server_cookie = 0;
  uint64_t global_seq = 0;
  uint64_t peer_global_seq = 0;
  uint64_t connect_seq = 0;

  seastar::future<> execution_done = seastar::now();

  class Timer {
    double last_dur_ = 0.0;
    const SocketConnection& conn;
    std::optional<seastar::abort_source> as;
   public:
    Timer(SocketConnection& conn) : conn(conn) {}
    double last_dur() const { return last_dur_; }
    seastar::future<> backoff(double seconds);
    void cancel() {
      last_dur_ = 0.0;
      if (as) {
        as->request_abort();
        as = std::nullopt;
      }
    }
  };
  Timer protocol_timer;
};

struct create_handlers_ret {
  std::unique_ptr<ConnectionHandler> io_handler;
  std::unique_ptr<ProtocolV2> protocol;
};
inline create_handlers_ret create_handlers(ChainedDispatchers &dispatchers, SocketConnection &conn) {
  std::unique_ptr<ConnectionHandler> io_handler = std::make_unique<IOHandler>(dispatchers, conn);
  IOHandler &io_handler_concrete = static_cast<IOHandler&>(*io_handler);
  auto protocol = std::make_unique<ProtocolV2>(conn, io_handler_concrete);
  io_handler_concrete.set_handshake_listener(*protocol);
  return {std::move(io_handler), std::move(protocol)};
}

} // namespace crimson::net

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<crimson::net::ProtocolV2> : fmt::ostream_formatter {};
#endif
