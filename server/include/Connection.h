#ifndef CONNECTION_H
#define CONNECTION_H

#include "Buffer.h"
#include "EventLoop.h"
#include "protocol/Message_codec.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <optional>
#include <string>

class Connection : public std::enable_shared_from_this<Connection> {
public:
    static constexpr size_t kReadBudgetBytes = 64 * 1024;
    static constexpr size_t kReadBudgetMessages = 64;
    using MessageCallback =
        std::function<void(const std::shared_ptr<Connection>& conn, const std::string&)>;

    enum class ConnState { Connected, Disconnecting, Disconnected };

public:
    struct ReadResult { //读结果
        bool ok{true};
        bool peer_close{false};
        bool decode_error{false};
        uint64_t bytes_received{0};
        uint64_t messages_decoded{0};
        uint64_t heartbeat_messages{0};
    };

    struct WriteResult { //写结果
        bool ok{true};
        bool close{false};
        uint64_t bytes_sent{0};
    };

public:
    explicit Connection(EventLoop* loop, int fd);
    EventLoop* ownerLoop() const;
    ~Connection();
    int fd() const;

    void setMessageCallback(MessageCallback cb);
    //处理读事件
    ReadResult handleRead();
    //处理写事件
    WriteResult handleWrite();
    //是否需要监听写事件
    bool wantWrite() const;
    void send(const std::string& data);
    // true means accepted into the owner loop, not delivered to the peer.
    bool sendPacket(const std::vector<char>& packet);
    // Counts queue acceptance only, never peer delivery. Dispatch packets are not responses.
    bool sendResponsePacket(const std::vector<char>& packet, bool heartbeat = false);
    void setResponseQueuedCallback(std::function<void(bool)> cb);
    void setLogIdentity(const std::string& role, const std::string& worker = "");
    std::string logContext() const;
    bool isWorkerConnection() const;
    void setWriteReadyCallback(std::function<void(const std::shared_ptr<Connection>&)> cb);

    ConnState state() const;
    void setState(ConnState st);
    bool isConnected() const;
    bool isDisconnecting() const;
    bool isDisconnected() const;

    void shutdown();
    bool shouldCloseAfterWrite() const;
    void incPendingTasks();
    void decPendingTasks();
    // Protocol messages are drained in wire order on a business worker thread.
    // 1 starts a drain, 0 joins one, -1 rejects an over-limit queue.
    int enqueueProtocolMessage(const std::string& message);
    std::optional<std::string> nextProtocolMessage();
    size_t abandonProtocolMessages();
    bool hasPendingTasks() const;
    bool canBeClosed() const;

    void refreshActivity();
    uint64_t lastActiveMs() const;

private:
    int fd_;
    uint64_t connection_id_{0};
    std::string peer_{"unknown"};
    mutable std::mutex identity_mutex_;
    std::string log_role_{"UNKNOWN"}, log_worker_;
    std::function<void(bool)> on_response_queued_;
    std::atomic<ConnState> state_;
    MessageCallback on_message_;
    std::function<void(const std::shared_ptr<Connection>&)> on_write_ready_;
    std::atomic<int> pending_packets_{0};

    Buffer inputBuffer_;
    Buffer outputBuffer_;

    std::atomic<int> pending_tasks_{0};
    std::mutex protocol_mutex_;
    std::deque<std::string> protocol_messages_;
    size_t protocol_bytes_{0};
    bool protocol_active_{false};

    std::atomic<uint64_t> last_active_ms_{0};

    EventLoop* owner_loop_;
};

#endif
