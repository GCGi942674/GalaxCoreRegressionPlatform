#include "ServerMetrics.h"

void ServerMetrics::onConnectionAccepted() {
    this->total_connections_accepted.inc();
    this->current_connections.inc();
}

void ServerMetrics::onConnectionClosed(CloseReason reason) {
    this->total_connections_closed.inc();
    this->current_connections.dec();
    switch (reason) {
        case CloseReason::PeerClosed:
            this->close_by_peer.inc();
            break;
        case CloseReason::IdleTimeout:
            this->close_by_idle_timeout.inc();
            break;
        case CloseReason::EpollError:
            this->close_by_epoll_error.inc();
            break;
        case CloseReason::ReadError:
            this->close_by_read_error.inc();
            break;
        case CloseReason::WriteError:
            this->close_by_write_error.inc();
            break;
        case CloseReason::ForceShutdown:
            this->close_by_shutdown_force.inc();
            break;
    }
}

void ServerMetrics::onBytesReceived(uint64_t n) {
    this->bytes_received.add(n);
}

void ServerMetrics::onBytesSent(uint64_t n) {
    this->bytes_sent.add(n);
}

void ServerMetrics::onMessageDecodedOk() {
    this->messages_decoded_ok.inc();
}

void ServerMetrics::onMessageDecodedInvalid() {
    this->messages_decoded_invalid.inc();
}

void ServerMetrics::onMessageReceived(bool heartbeat) {
    this->messages_received.inc();
    if (heartbeat) {
        this->heartbeat_requests.inc();
    }
}

void ServerMetrics::onResponseSent(bool heartbeat) {
    this->responses_sent.inc();
    if (heartbeat) {
        this->heartbeat_responses.inc();
    }
}

void ServerMetrics::onWorkerTaskSubmitted() {
    this->worker_tasks_submitted.inc();
    this->worker_tasks_inflight.inc();
}

void ServerMetrics::onWorkerTaskRejected() {
    this->worker_tasks_rejected.inc();
}

void ServerMetrics::onWorkerTaskCompleted(uint64_t business_cost_us) {
    this->worker_tasks_completed.inc();
    this->worker_tasks_inflight.dec();
    this->business_latency_us_total.add(business_cost_us);
    this->business_latency_us_max.updateMax(business_cost_us);
}

ServerMetricsSnapshot ServerMetrics::snapshot() const {
    ServerMetricsSnapshot snapshot;

    snapshot.current_connections = this->current_connections.load();
    snapshot.total_connections_closed = this->total_connections_closed.load();
    snapshot.total_connections_accepted = this->total_connections_accepted.load();
    snapshot.close_by_peer = this->close_by_peer.load();
    snapshot.close_by_idle_timeout = this->close_by_idle_timeout.load();
    snapshot.close_by_epoll_error = this->close_by_epoll_error.load();
    snapshot.close_by_read_error = this->close_by_read_error.load();
    snapshot.close_by_write_error = this->close_by_write_error.load();
    snapshot.close_by_shutdown_force = this->close_by_shutdown_force.load();
    snapshot.messages_received = this->messages_received.load();
    snapshot.messages_decoded_ok = this->messages_decoded_ok.load();
    snapshot.messages_decoded_invalid = this->messages_decoded_invalid.load();
    snapshot.responses_sent = this->responses_sent.load();
    snapshot.heartbeat_requests = this->heartbeat_requests.load();
    snapshot.heartbeat_responses = this->heartbeat_responses.load();
    snapshot.bytes_received = this->bytes_received.load();
    snapshot.bytes_sent = this->bytes_sent.load();
    snapshot.worker_tasks_submitted = this->worker_tasks_submitted.load();
    snapshot.worker_tasks_rejected = this->worker_tasks_rejected.load();
    snapshot.worker_tasks_completed = this->worker_tasks_completed.load();
    snapshot.worker_tasks_inflight = this->worker_tasks_inflight.load();
    snapshot.business_latency_us_total = this->business_latency_us_total.load();
    snapshot.business_latency_us_max = this->business_latency_us_max.load();

    return snapshot;
}
