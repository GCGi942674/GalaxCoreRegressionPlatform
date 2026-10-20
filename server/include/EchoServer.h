#ifndef ECHO_SERVER_H
#define ECHO_SERVER_H

#include "Acceptor.h"
#include "Connection.h"
#include "EchoHandler.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "ServerMetrics.h"
#include "Thread_pool.h"
#include "protocol/Message_codec.h"
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/epoll.h>
#include <unordered_map>

class EchoServer {
public:
  EchoServer(int port, EchoHandler &handler, int signal_fd,
             size_t io_thread_num, size_t worker_thread_num = 8,
             std::string bind_ip = "0.0.0.0");
  ~EchoServer();
  bool run();
  void beginShutdown();
  void tryFinishShutdown();
  void onMessage(const std::shared_ptr<Connection> &conn,
                 const std::string &msg);
  using ConnectionCallback = std::function<void(const std::shared_ptr<Connection>&)>;
  void setConnectionCallbacks(ConnectionCallback opened,
      Connection::MessageCallback message, ConnectionCallback closed);

private:
  void handleNewConnection(int client_fd);
  void handleClientEvent(const std::shared_ptr<Connection>& conn, uint32_t events);
  void removeConnection(const std::shared_ptr<Connection>& conn, ServerMetrics::CloseReason reason);
  void updateConnectionEvent(const std::shared_ptr<Connection>& conn, bool want_write);
  std::vector<std::shared_ptr<Connection>> connectionSnapshot();

private:
  uint64_t idle_timeout_ms_{60000};
  uint64_t shutdown_timeout_ms_{30000};
  EventLoop::TimerId shutdown_timer_{0};
  EventLoop::TimerId idle_check_timer_{0};

  EchoHandler &handler_;
  EventLoop loop_;
  Acceptor acceptor_;

  std::unique_ptr<EventLoopThreadPool> io_loop_pool_;
  size_t io_thread_num_;

  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  std::mutex connection_mutex_;
  ConnectionCallback connection_opened_;
  Connection::MessageCallback protocol_message_;
  ConnectionCallback connection_closed_;
  std::atomic<size_t> pending_accepts_{0};

  ThreadPool pool_;
  std::atomic<bool> stopping_{false};
  int signal_fd{-1};

  ServerMetrics metrics_;
  ServerMetricsSnapshot last_metrics_snapshot_;
  bool has_last_snapshot_{false};

  uint64_t metrics_interval_ms_{60000};
  uint64_t last_metrics_ms_{0};
};

#endif // ECHO_SERVER_H
