#include "GcrpClient.h"
#include "protocol/ConnectionRole.h"
#include "protocol/ProtocolCodec.h"
#include "protocol/Message_codec.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace GCRP {
namespace {
using Clock = std::chrono::steady_clock;

struct Socket {
    int fd;

    ~Socket() {
        if (fd >= 0)
            ::close(fd);
    }
};

[[noreturn]] void ioError(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

void waitFor(int fd, short events, Clock::time_point deadline) {
    while (true) {
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0)
            throw std::runtime_error("request deadline exceeded");
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            ioError("poll");
        if (result == 0)
            throw std::runtime_error("request deadline exceeded");
        if (descriptor.revents & POLLNVAL)
            throw std::runtime_error("invalid socket");
        if (descriptor.revents & (events | POLLERR | POLLHUP))
            return;
    }
}

void transfer(int fd, char* data, size_t size, bool sending, Clock::time_point deadline) {
    while (size) {
        waitFor(fd, sending ? POLLOUT : POLLIN, deadline);
        const auto n = sending ? ::send(fd, data, size, MSG_NOSIGNAL) : ::recv(fd, data, size, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n < 0)
            ioError(sending ? "send" : "recv");
        if (n == 0)
            throw std::runtime_error("connection closed before response completed");
        data += n;
        size -= static_cast<size_t>(n);
    }
}
} // namespace

GcrpClient::GcrpClient(std::string host, uint16_t port, int timeout_ms)
    : host_(std::move(host))
    , port_(port)
    , timeout_ms_(timeout_ms) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, host_.c_str(), &parsed) != 1)
        throw std::invalid_argument("host must be a numeric IPv4 address");
    if (!port_ || timeout_ms_ < 1 || timeout_ms_ > 60000)
        throw std::invalid_argument("invalid port or timeout (1..60000 ms)");
}

Protocol::ProtocolMessage GcrpClient::request(
    Protocol::MessageType type, const nlohmann::json& data) {
    using namespace Protocol;
    if (!isClientRequest(type) || !data.is_object())
        throw std::invalid_argument("not a client request");
    if (next_request_id_ == 0)
        throw std::overflow_error("request id exhausted");
    ProtocolMessage request;
    request.type = type;
    request.request_id = next_request_id_++;
    request.data = data;
    ProtocolError error;
    std::string text;
    if (!ProtocolCodec::encode(request, text, error))
        throw std::runtime_error(error.message);
    auto packet = MessageCodec::encode(text);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
    Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (socket.fd < 0)
        ioError("socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    ::inet_pton(AF_INET, host_.c_str(), &address.sin_addr);
    if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS)
            ioError("connect");
        waitFor(socket.fd, POLLOUT, deadline);
        int status = 0;
        socklen_t length = sizeof(status);
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &status, &length) < 0)
            ioError("connect status");
        if (status) {
            errno = status;
            ioError("connect");
        }
    }
    transfer(socket.fd, packet.data(), packet.size(), true, deadline);
    uint32_t length = 0;
    transfer(socket.fd, reinterpret_cast<char*>(&length), sizeof(length), false, deadline);
    length = ntohl(length);
    if (length > MessageCodec::kMaxBodyLenght)
        throw std::runtime_error("response frame exceeds size limit");
    std::string body(length, '\0');
    transfer(socket.fd, body.data(), body.size(), false, deadline);
    ProtocolMessage response;
    if (!ProtocolCodec::decode(body, response, error))
        throw std::runtime_error("invalid response: " + error.message);
    if (response.type != MessageType::RESPONSE || response.request_id != request.request_id)
        throw std::runtime_error("response type or request_id mismatch");
    const auto& value = response.data;
    if (!value.contains("code") || !value["code"].is_number_integer() || value["code"] < 0 ||
        value["code"] > std::numeric_limits<int>::max() || !value.contains("message") ||
        !value["message"].is_string() || !value.contains("result") ||
        !(value["result"].is_object() || value["result"].is_null()))
        throw std::runtime_error("invalid response payload");
    return response;
}

Protocol::ProtocolMessage GcrpClient::submit(
    const std::string& target, const std::string& revision, int priority) {
    return request(Protocol::MessageType::SUBMIT_TASK,
        {{"task_type", "REGRESSION"},
            {"target", target},
            {"revision", revision},
            {"priority", priority}});
}

Protocol::ProtocolMessage GcrpClient::query(uint64_t id) {
    return request(Protocol::MessageType::QUERY_TASK, {{"task_id", id}});
}

Protocol::ProtocolMessage GcrpClient::cancel(uint64_t id) {
    return request(Protocol::MessageType::CANCEL_TASK, {{"task_id", id}});
}

Protocol::ProtocolMessage GcrpClient::queryBatch(const std::string& id) {
    return request(Protocol::MessageType::QUERY_BATCH, {{"batch_id", id}});
}

Protocol::ProtocolMessage GcrpClient::cancelBatch(const std::string& id) {
    return request(Protocol::MessageType::CANCEL_BATCH, {{"batch_id", id}});
}

Protocol::ProtocolMessage GcrpClient::queryRegression(const nlohmann::json& filter) {
    return request(Protocol::MessageType::QUERY_REGRESSION, filter);
}

Protocol::ProtocolMessage GcrpClient::queryProgress(const nlohmann::json& filter) {
    return request(Protocol::MessageType::QUERY_PROGRESS, filter);
}

Protocol::ProtocolMessage GcrpClient::queryRollback(const nlohmann::json& filter) {
    return request(Protocol::MessageType::QUERY_ROLLBACK, filter);
}
} // namespace GCRP
