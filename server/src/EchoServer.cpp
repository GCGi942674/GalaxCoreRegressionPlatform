#include "EchoServer.h"
#include "Logging.h"
#include "Scope_guard.h"
#include "Utils.h" // 来自 common
#include "config/Config.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace GCRP;

EchoServer::EchoServer(int port,
    EchoHandler& handler,
    int signal_fd,
    size_t io_thread_num,
    size_t worker_thread_num,
    std::string bind_ip)
    : handler_(handler)
    , loop_()
    , acceptor_(&loop_, port, std::move(bind_ip))
    , io_loop_pool_(std::make_unique<EventLoopThreadPool>(io_thread_num))
    , io_thread_num_(io_thread_num)
    , pool_(worker_thread_num)
    , signal_fd(signal_fd) {
    const int interval = Config::instance().getInt("log.metrics_interval_seconds", 60);
    if (interval < 5 || interval > 3600)
        throw std::runtime_error("log.metrics_interval_seconds must be 5..3600");
    metrics_interval_ms_ = static_cast<uint64_t>(interval) * 1000;
    std::ostringstream oss;
    oss << "EchoServer created, port=" << port;
    LOG_INFO(LogModule::SERVER, oss.str());
}

EchoServer::~EchoServer() {
    pool_.stop();
    io_loop_pool_->stop();
}

void EchoServer::setConnectionCallbacks(
    ConnectionCallback opened, Connection::MessageCallback message, ConnectionCallback closed) {
    connection_opened_ = std::move(opened);
    protocol_message_ = std::move(message);
    connection_closed_ = std::move(closed);
}

std::vector<std::shared_ptr<Connection>> EchoServer::connectionSnapshot() {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    std::vector<std::shared_ptr<Connection>> result;
    for (const auto& item : connections_)
        result.push_back(item.second);
    return result;
}

void EchoServer::onMessage(const std::shared_ptr<Connection>& conn, const std::string& msg) {
    if (protocol_message_) {
        int queued = -1;
        try {
            queued = conn->enqueueProtocolMessage(msg);
        } catch (...) {
            conn->decPendingTasks();
            conn->setState(Connection::ConnState::Disconnected);
            removeConnection(conn, ServerMetrics::CloseReason::ReadError);
            return;
        }
        if (queued < 0) {
            conn->decPendingTasks();
            conn->setState(Connection::ConnState::Disconnected);
            removeConnection(conn, ServerMetrics::CloseReason::ReadError);
            return;
        }
        if (queued == 0)
            return;
        bool accepted = false;
        try {
            accepted = pool_.addTask([this, conn]() {
                while (auto next = conn->nextProtocolMessage()) {
                    auto finished = finally([&] {
                        conn->decPendingTasks();
                    });
                    if (conn->isDisconnected())
                        continue;
                    try {
                        protocol_message_(conn, *next);
                    } catch (...) {
                        conn->setState(Connection::ConnState::Disconnected);
                    }
                }
                conn->ownerLoop()->tryQueueInLoop([this, conn]() {
                    if (conn->isDisconnected())
                        removeConnection(conn, ServerMetrics::CloseReason::ReadError);
                    else if (conn->canBeClosed())
                        removeConnection(conn, ServerMetrics::CloseReason::PeerClosed);
                });
            });
        } catch (...) {
        }
        if (!accepted) {
            const auto dropped = conn->abandonProtocolMessages();
            for (size_t i = 0; i < dropped; ++i)
                conn->decPendingTasks();
            conn->setState(Connection::ConnState::Disconnected);
            removeConnection(conn, ServerMetrics::CloseReason::ReadError);
        }
        return;
    }
    std::weak_ptr<Connection> weak_conn = conn;

    bool ok = this->pool_.addTask([this, weak_conn, msg]() -> void {
        bool is_heartbeat = (msg == "__ping__");
        uint64_t begin_us = getSteadyClockUs();

        if (is_heartbeat) {
            LOG_DEBUG(LogModule::SERVER, "heartbeat pong queued");
        } else {
            LOG_DEBUG(LogModule::SERVER, "normal response queued");
        }

        auto resp = this->handler_.onMessage(msg);
        auto packet = MessageCodec::encode(resp);

        uint64_t end_us = getSteadyClockUs();
        this->metrics_.onWorkerTaskCompleted(end_us - begin_us);

        auto conn = weak_conn.lock();
        if (!conn) {
            LOG_WARN(LogModule::SERVER, "Connection expired before sending response");
            return;
        }

        EventLoop* onwer = conn->ownerLoop();

        onwer->queueInLoop([this, weak_conn, packet, is_heartbeat]() -> void {
            auto conn = weak_conn.lock();
            if (!conn) {
                LOG_WARN(LogModule::SERVER, "Connection expired before sending response");
                return;
            }

            auto guard = finally([&] {
                conn->decPendingTasks();
            });

            int fd = conn->fd();
            std::unordered_map<int, std::shared_ptr<Connection>>::iterator iter;
            {
                std::lock_guard<std::mutex> lock(this->connection_mutex_);
                iter = this->connections_.find(fd);
                if (iter == this->connections_.end()) {
                    std::ostringstream oss;
                    oss << "connection not found when sending response, fd=" << fd;
                    LOG_WARN(LogModule::SERVER, oss.str());
                    return;
                }
                if (iter->second != conn)
                    return;
            }
            if (conn->isDisconnected()) {
                std::ostringstream oss;
                oss << "fd reused, skip stale connection response, fd=" << fd;
                LOG_WARN(LogModule::SERVER, oss.str());
                return;
            }

            if (!conn->isConnected() && !conn->isDisconnecting()) {
                std::ostringstream oss;
                oss << "connection already full disconnected, fd=" << fd;
                LOG_WARN(LogModule::SERVER, oss.str());
                return;
            }

            conn->sendResponsePacket(packet, is_heartbeat);
            this->updateConnectionEvent(conn, conn->wantWrite());
            std::ostringstream oss;
            oss << "response queued back to loop, fd=" << fd
                << ", want_write=" << conn->wantWrite();
            LOG_DEBUG(LogModule::SERVER, oss.str());

            if (conn->canBeClosed()) {
                this->removeConnection(conn, ServerMetrics::CloseReason::PeerClosed);
            }
        });
    });

    if (!ok) {
        conn->decPendingTasks();
        this->metrics_.onWorkerTaskRejected();
        LOG_WARN(LogModule::SERVER, "thread pool shutting down, reject new task");
    } else {
        this->metrics_.onWorkerTaskSubmitted();
    }
}

bool EchoServer::run() {
    LOG_INFO(LogModule::SERVER, "EchoServer running");

    this->io_loop_pool_->start();

    this->loop_.addFd(signal_fd, EPOLLIN, [this](uint32_t) {
        uint64_t val = 0;
        while (true) {
            ssize_t n = ::read(this->signal_fd, &val, sizeof(val));
            if (n > 0) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        LOG_INFO(LogModule::SERVER, "signal received, begin graceful shutdown");
        this->beginShutdown();
    });

    this->acceptor_.setNewConnectionCallback([this](int client_fd) {
        this->handleNewConnection(client_fd);
    });

    if (!this->acceptor_.startListen()) {
        LOG_ERROR(LogModule::SERVER, "acceptor startListen failed, quit server");
        this->loop_.quit();
        return false;
    }

    last_metrics_ms_ = getSteadyClockMs();
    last_metrics_snapshot_ = metrics_.snapshot();
    has_last_snapshot_ = true;
    this->idle_check_timer_ = this->loop_.runEvery(5000, [this]() {
        const auto metrics_now = getSteadyClockMs();
        if (metrics_now - last_metrics_ms_ >= metrics_interval_ms_) {
            auto snap = this->metrics_.snapshot();

            double qps = 0.0;
            double in_bytes_per_sec = 0.0;
            double out_bytes_per_sec = 0.0;
            double avg_latency_us = 0.0;

            uint64_t delta_msg = 0;
            uint64_t delta_in = 0;
            uint64_t delta_out = 0;
            uint64_t delta_done = 0;
            uint64_t delta_latency = 0;
            uint64_t delta_accept = 0;
            uint64_t delta_close = 0;

            const double interval_sec =
                static_cast<double>(metrics_now - last_metrics_ms_) / 1000.0;

            if (this->has_last_snapshot_) {
                delta_msg = snap.messages_received - this->last_metrics_snapshot_.messages_received;
                delta_in = snap.bytes_received - this->last_metrics_snapshot_.bytes_received;
                delta_out = snap.bytes_sent - this->last_metrics_snapshot_.bytes_sent;
                delta_done = snap.worker_tasks_completed -
                             this->last_metrics_snapshot_.worker_tasks_completed;
                delta_latency = snap.business_latency_us_total -
                                this->last_metrics_snapshot_.business_latency_us_total;
                delta_accept = snap.total_connections_accepted -
                               this->last_metrics_snapshot_.total_connections_accepted;
                delta_close = snap.total_connections_closed -
                              this->last_metrics_snapshot_.total_connections_closed;

                qps = static_cast<double>(delta_msg) / interval_sec;
                in_bytes_per_sec = static_cast<double>(delta_in) / interval_sec;
                out_bytes_per_sec = static_cast<double>(delta_out) / interval_sec;

                avg_latency_us =
                    (delta_done > 0)
                        ? (static_cast<double>(delta_latency) / static_cast<double>(delta_done))
                        : 0.0;
            }

            std::ostringstream oss;
            oss << "[METRICS] window_s=" << interval_sec << " conn_cur=" << snap.current_connections
                << " conn_total=" << snap.total_connections_accepted
                << " conn_closed=" << snap.total_connections_closed
                << " msg_recv=" << snap.messages_received << " resp_queued=" << snap.responses_sent
                << " bytes_in=" << snap.bytes_received << " bytes_out=" << snap.bytes_sent
                << " accept=" << delta_accept << " close=" << delta_close
                << " recv_msg_per_s=" << qps << " in_Bps=" << in_bytes_per_sec
                << " out_Bps=" << out_bytes_per_sec;
            // Echo job counters do not describe scheduler testcase completion.
            if (!protocol_message_)
                oss << " echo_jobs_done=" << snap.worker_tasks_completed
                    << " echo_avg_us=" << avg_latency_us;
            LOG_INFO(LogModule::SERVER, oss.str());

            this->last_metrics_snapshot_ = snap;
            this->has_last_snapshot_ = true;
            last_metrics_ms_ = metrics_now;
        }

        uint64_t now = getSteadyClockMs();

        for (const auto& conn : connectionSnapshot()) {
            conn->ownerLoop()->queueInLoop([this, conn, now]() {
                if (conn->isDisconnected()) {
                    removeConnection(conn, ServerMetrics::CloseReason::IdleTimeout);
                    return;
                }
                if (now >= conn->lastActiveMs() && now - conn->lastActiveMs() > idle_timeout_ms_) {
                    conn->shutdown();
                    updateConnectionEvent(conn, conn->wantWrite());
                    if (conn->canBeClosed())
                        removeConnection(conn, ServerMetrics::CloseReason::IdleTimeout);
                }
            });
        }
    });

    this->loop_.loop();
    return true;
}

void EchoServer::beginShutdown() {
    if (this->stopping_.exchange(true)) {
        return;
    }

    LOG_INFO(LogModule::SERVER, "begin graceful shutdown");

    this->acceptor_.stopListen();
    this->pool_.shutdown();

    for (const auto& conn : connectionSnapshot()) {
        conn->ownerLoop()->queueInLoop([this, conn]() {
            conn->shutdown();
            updateConnectionEvent(conn, conn->wantWrite());
            if (conn->canBeClosed())
                removeConnection(conn, ServerMetrics::CloseReason::PeerClosed);
        });
    }

    this->shutdown_timer_ = this->loop_.runAfter(this->shutdown_timeout_ms_, [this]() {
        LOG_WARN(LogModule::SERVER, "graceful shutdown timeout, force closing all connections");

        auto fds = connectionSnapshot();

        for (auto& conn : fds) {
            this->removeConnection(conn, ServerMetrics::CloseReason::ForceShutdown);
        }
    });

    this->tryFinishShutdown();
}

void EchoServer::tryFinishShutdown() {
    if (!loop_.isInLoopThread()) {
        loop_.queueInLoop([this]() {
            tryFinishShutdown();
        });
        return;
    }
    if (this->stopping_ && pending_accepts_ == 0 && connectionSnapshot().empty()) {
        if (this->shutdown_timer_ != 0) {
            this->loop_.cancelTimer(this->shutdown_timer_);
            this->shutdown_timer_ = 0;
        }

        LOG_INFO(
            LogModule::SERVER, "all connections drained, stopping thread pool and quitting loop");
        this->pool_.stop();
        this->loop_.quit();
    }
}

void EchoServer::handleClientEvent(const std::shared_ptr<Connection>& conn, uint32_t events) {
    int client_fd = conn->fd();
    std::ostringstream oss;
    oss << "handle client event, fd=" << client_fd << ", events=" << events;
    LOG_DEBUG(LogModule::NETWORK, oss.str());
    std::unordered_map<int, std::shared_ptr<Connection>>::iterator iter;
    {
        std::lock_guard<std::mutex> lock(this->connection_mutex_);
        iter = this->connections_.find(client_fd);
        if (iter == this->connections_.end()) {
            std::ostringstream oss;
            oss << "client event but connection not found, fd=" << client_fd;
            LOG_DEBUG(LogModule::NETWORK, oss.str());
            return;
        }
    }

    if (conn->isDisconnected()) {
        std::ostringstream oss;
        oss << "connection already disconnected before handling event, fd=" << client_fd;
        LOG_DEBUG(LogModule::NETWORK, oss.str());
        this->removeConnection(conn, ServerMetrics::CloseReason::PeerClosed);
        return;
    }

    if (events & (EPOLLERR | EPOLLHUP)) {
        std::ostringstream oss;
        oss << "epoll error/hup, remove connection, fd=" << client_fd << ", events=" << events;
        LOG_DEBUG(LogModule::NETWORK, oss.str());
        this->removeConnection(conn, ServerMetrics::CloseReason::EpollError);
        return;
    }

    // RDHUP does not mean the receive queue is drained. With bounded reads,
    // continue across turns until recv returns EOF; only then enter shutdown.
    if (events & (EPOLLIN | EPOLLRDHUP)) {
        auto rr = conn->handleRead();
        this->metrics_.onBytesReceived(rr.bytes_received);

        for (uint64_t i = 0; i < rr.messages_decoded; ++i) {
            this->metrics_.onMessageDecodedOk();
        }

        for (uint64_t i = 0; i < rr.heartbeat_messages; ++i) {
            this->metrics_.onMessageReceived(true);
        }

        uint64_t normal_msgs = rr.messages_decoded - rr.heartbeat_messages;
        for (uint64_t i = 0; i < normal_msgs; ++i) {
            this->metrics_.onMessageReceived(false);
        }

        if (rr.peer_close && conn->canBeClosed()) {
            this->removeConnection(conn, ServerMetrics::CloseReason::PeerClosed);
            return;
        }

        if (rr.decode_error) {
            this->removeConnection(conn, ServerMetrics::CloseReason::ReadError);
            std::ostringstream oss;
            oss << "handleRead failed, remove connection, fd=" << client_fd;
            LOG_DEBUG(LogModule::NETWORK, oss.str());
            return;
        }

        if (!rr.ok) {
            this->removeConnection(conn, ServerMetrics::CloseReason::ReadError);
            std::ostringstream oss;
            oss << "handleRead failed, remove connection, fd=" << client_fd;
            LOG_DEBUG(LogModule::NETWORK, oss.str());
            return;
        }
    }

    if (events & EPOLLOUT) {
        auto wr = conn->handleWrite();
        this->metrics_.onBytesSent(wr.bytes_sent);
        if (!wr.ok) {
            std::ostringstream oss;
            oss << "handleWrite failed, remove connection, fd=" << client_fd;
            LOG_DEBUG(LogModule::NETWORK, oss.str());
            this->removeConnection(conn, ServerMetrics::CloseReason::WriteError);
            return;
        }

        if (wr.close) {
            this->removeConnection(conn, ServerMetrics::CloseReason::PeerClosed);
            return;
        }
    }

    this->updateConnectionEvent(conn, conn->wantWrite());
}

void EchoServer::handleNewConnection(int client_fd) {
    if (this->stopping_) {
        std::ostringstream oss;
        oss << "server stopping, reject new connection, fd = " << client_fd;
        LOG_INFO(LogModule::SERVER, oss.str());
        close(client_fd);
        return;
    }

    EventLoop* io_loop = this->io_loop_pool_->getNextLoop();
    if (io_loop == nullptr) {
        io_loop = &this->loop_;
    }

    ++pending_accepts_;
    io_loop->queueInLoop([this, io_loop, client_fd]() {
        auto accepted = finally([this]() {
            --pending_accepts_;
            tryFinishShutdown();
        });
        if (stopping_) {
            ::close(client_fd);
            return;
        }
        std::shared_ptr<Connection> conn;
        {
            std::lock_guard<std::mutex> lock(this->connection_mutex_);
            conn = std::make_shared<Connection>(io_loop, client_fd);
            this->connections_.emplace(client_fd, conn);
        }

        this->metrics_.onConnectionAccepted();
        conn->setResponseQueuedCallback([this](bool heartbeat) {
            metrics_.onResponseSent(heartbeat);
        });

        conn->setWriteReadyCallback([this](const std::shared_ptr<Connection>& current) {
            if (current->isDisconnected()) {
                removeConnection(current, ServerMetrics::CloseReason::WriteError);
            } else {
                updateConnectionEvent(current, current->wantWrite());
            }
        });
        if (connection_opened_)
            connection_opened_(conn);

        conn->setMessageCallback(
            [this](const std::shared_ptr<Connection>& conn, const std::string& msg) {
                this->onMessage(conn, msg);
            });

        io_loop->addFd(client_fd, EPOLLIN | EPOLLRDHUP, [this, conn](uint32_t events) {
            this->handleClientEvent(conn, events);
        });
        std::ostringstream oss;
        oss << "new connection registered, " << conn->logContext()
            << ", current_connections=" << this->metrics_.snapshot().current_connections;
        LOG_DEBUG(LogModule::SERVER, oss.str());
    });
}

void EchoServer::removeConnection(
    const std::shared_ptr<Connection>& conn, ServerMetrics::CloseReason reason) {

    if (!conn) {
        return;
    }

    EventLoop* loop = conn->ownerLoop();

    loop->queueInLoop([this, conn, reason]() {
        int client_fd = conn->fd();
        {
            std::lock_guard<std::mutex> lock(this->connection_mutex_);

            auto iter = this->connections_.find(client_fd);
            if (iter == this->connections_.end()) {
                std::ostringstream oss;
                oss << "removeConnection: fd not found, fd=" << client_fd;
                LOG_DEBUG(LogModule::SERVER, oss.str());
                return;
            }

            if (iter->second != conn) {
                std::ostringstream oss;
                oss << "removeConnection: fd reused, fd=" << client_fd;
                LOG_DEBUG(LogModule::SERVER, oss.str());
                return;
            }

            this->connections_.erase(client_fd);
        }
        conn->setState(Connection::ConnState::Disconnected);
        conn->ownerLoop()->removeFd(client_fd);
        this->metrics_.onConnectionClosed(reason);
        if (connection_closed_)
            connection_closed_(conn);
        std::ostringstream oss;
        const char* why = "unknown";
        switch (reason) {
            case ServerMetrics::CloseReason::PeerClosed:
                why = "peer_closed";
                break;
            case ServerMetrics::CloseReason::IdleTimeout:
                why = "idle_timeout";
                break;
            case ServerMetrics::CloseReason::EpollError:
                why = "epoll_error_or_hup";
                break;
            case ServerMetrics::CloseReason::ReadError:
                why = "read_or_protocol_error";
                break;
            case ServerMetrics::CloseReason::WriteError:
                why = "write_error";
                break;
            case ServerMetrics::CloseReason::ForceShutdown:
                why = "server_shutdown";
                break;
        }
        oss << "connection closed, " << conn->logContext() << " reason=" << why
            << " current_connections=" << this->metrics_.snapshot().current_connections;
        if (reason == ServerMetrics::CloseReason::ReadError ||
            reason == ServerMetrics::CloseReason::WriteError ||
            reason == ServerMetrics::CloseReason::EpollError)
            LOG_WARN(LogModule::NETWORK, oss.str());
        else if (conn->isWorkerConnection())
            LOG_INFO(LogModule::WORKER, oss.str());
        else
            LOG_DEBUG(LogModule::NETWORK, oss.str());
        this->tryFinishShutdown();
    });
}

void EchoServer::updateConnectionEvent(
    const std::shared_ptr<Connection>& conn, bool /*want_write*/) {
    if (!conn->ownerLoop()->isInLoopThread()) {
        conn->ownerLoop()->queueInLoop([this, conn]() {
            updateConnectionEvent(conn, false);
        });
        return;
    }
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        auto it = connections_.find(conn->fd());
        if (it == connections_.end() || it->second != conn)
            return;
    }
    uint32_t events = EPOLLRDHUP;
    if (!stopping_ && conn->isConnected())
        events |= EPOLLIN;
    if (conn->wantWrite())
        events |= EPOLLOUT;
    conn->ownerLoop()->updateFd(conn->fd(), events);
}
