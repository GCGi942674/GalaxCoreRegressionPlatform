#ifndef SERVERMETRICS_H_
#define SERVERMETRICS_H_

#include "MetricCounter.h"
#include <cstdint>

struct ServerMetricsSnapshot {
  uint64_t total_connections_accepted{0};
  int64_t current_connections{0};
  uint64_t total_connections_closed{0};

  uint64_t close_by_peer{0};
  uint64_t close_by_idle_timeout{0};
  uint64_t close_by_epoll_error{0};
  uint64_t close_by_read_error{0};
  uint64_t close_by_write_error{0};
  uint64_t close_by_shutdown_force{0};

  uint64_t messages_received{0};
  uint64_t messages_decoded_ok{0};
  uint64_t messages_decoded_invalid{0};
  uint64_t responses_sent{0}; // Legacy field name: responses accepted into send queue, not delivery ACKs.

  uint64_t heartbeat_requests{0};
  uint64_t heartbeat_responses{0};

  uint64_t bytes_received{0};
  uint64_t bytes_sent{0};

  uint64_t worker_tasks_submitted{0};
  uint64_t worker_tasks_rejected{0};
  uint64_t worker_tasks_completed{0};
  int64_t worker_tasks_inflight{0};

  uint64_t business_latency_us_total{0};
  uint64_t business_latency_us_max{0};
};

class ServerMetrics {
public:
  enum class CloseReason {
    PeerClosed,
    IdleTimeout,
    EpollError,
    ReadError,
    WriteError,
    ForceShutdown
  };

public:
  void onConnectionAccepted();
  void onConnectionClosed(CloseReason reason);

  void onBytesReceived(uint64_t n);
  void onBytesSent(uint64_t n);

  void onMessageDecodedOk();
  void onMessageDecodedInvalid();
  void onMessageReceived(bool heartbeat);

  void onResponseSent(bool heartbeat);

  void onWorkerTaskSubmitted();
  void onWorkerTaskRejected();
  void onWorkerTaskCompleted(uint64_t business_cost_us);

  ServerMetricsSnapshot snapshot() const;

private:
  MetricCounter total_connections_accepted;
  Gauge current_connections;
  MetricCounter total_connections_closed;

  MetricCounter close_by_peer;
  MetricCounter close_by_idle_timeout;
  MetricCounter close_by_epoll_error;
  MetricCounter close_by_read_error;
  MetricCounter close_by_write_error;
  MetricCounter close_by_shutdown_force;

  MetricCounter messages_received;
  MetricCounter messages_decoded_ok;
  MetricCounter messages_decoded_invalid;
  MetricCounter responses_sent;

  MetricCounter heartbeat_requests;
  MetricCounter heartbeat_responses;

  MetricCounter bytes_received;
  MetricCounter bytes_sent;

  MetricCounter worker_tasks_submitted;
  MetricCounter worker_tasks_rejected;
  MetricCounter worker_tasks_completed;
  Gauge worker_tasks_inflight;

  MetricCounter business_latency_us_total;
  MetricCounter business_latency_us_max;
};

#endif
