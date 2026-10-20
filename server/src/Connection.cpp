#include "Connection.h"
#include "Logging.h"
#include "Utils.h"
#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

using namespace GCRP;

Connection::Connection(EventLoop *loop, int fd)
    : owner_loop_(loop), fd_(fd), state_(ConnState::Connected) {
  static std::atomic<uint64_t> next_connection_id{0};connection_id_=++next_connection_id;
  sockaddr_in address{};socklen_t length=sizeof(address);char ip[INET_ADDRSTRLEN]{};
  if(::getpeername(fd,reinterpret_cast<sockaddr*>(&address),&length)==0&&address.sin_family==AF_INET&&
     ::inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip)))peer_=std::string(ip)+":"+std::to_string(ntohs(address.sin_port));
  this->refreshActivity();
  std::ostringstream oss;
  oss << "connection created, fd=" << this->fd_;
  LOG_DEBUG(LogModule::NETWORK, oss.str());
}

EventLoop *Connection::ownerLoop() const { return this->owner_loop_; }

Connection::~Connection() {
  if (this->fd_ != -1) {
    close(this->fd_);
    this->fd_ = -1;
  }
}

int Connection::fd() const { return fd_; }

Connection::ReadResult Connection::handleRead() {
  ReadResult result;
  char buffer[4096];
  // EchoServer uses level-triggered epoll: unread kernel data schedules another turn.
  while (result.messages_decoded < kReadBudgetMessages) {
    size_t frame_size = MessageCodec::kHeaderLength;
    if (inputBuffer_.readableBytes() >= MessageCodec::kHeaderLength) {
      uint32_t body_size;
      std::memcpy(&body_size, inputBuffer_.peek(), sizeof(body_size));
      body_size = ntohl(body_size);
      if (body_size > MessageCodec::kMaxBodyLenght) {
        setState(ConnState::Disconnected);
        result.ok = false;
        result.decode_error = true;
        return result;
      }
      frame_size += body_size;
      if (inputBuffer_.readableBytes() == frame_size) {
        std::string message;
        const auto decoded = MessageCodec::Decoder::tryDecode(inputBuffer_, message);
        if (decoded != MessageCodec::DecodeResult::Ok) {
          setState(ConnState::Disconnected);
          result.ok = false;
          result.decode_error = true;
          return result;
        }
        ++result.messages_decoded;
        if (message == "__ping__") ++result.heartbeat_messages;
        if (on_message_) {
          incPendingTasks();
          on_message_(shared_from_this(), message);
        }
        if (isDisconnected()) return result;
        continue;
      }
    }
    if (result.bytes_received >= kReadBudgetBytes) break;
    // Never read past this frame. The input buffer holds at most one bounded frame.
    const size_t amount = std::min({sizeof(buffer),
        frame_size - inputBuffer_.readableBytes(),
        kReadBudgetBytes - static_cast<size_t>(result.bytes_received)});
    const ssize_t n = ::recv(fd_, buffer, amount, 0);
    if (n > 0) {
      refreshActivity();
      inputBuffer_.append(buffer, static_cast<size_t>(n));
      result.bytes_received += static_cast<size_t>(n);
    } else if (n == 0) {
      shutdown();
      result.peer_close = true;
      break;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      setState(ConnState::Disconnected);
      result.ok = false;
      return result;
    }
  }
  return result;
}


Connection::WriteResult Connection::handleWrite() {
  WriteResult write_res;

  while (this->outputBuffer_.readableBytes() > 0) {

    ssize_t n = ::send(this->fd_, this->outputBuffer_.peek(),
                       this->outputBuffer_.readableBytes(), MSG_NOSIGNAL);
    if (n > 0) {
      this->refreshActivity();
      std::ostringstream oss;
      oss << "send success, fd=" << this->fd_ << ", bytes=" << n;
      LOG_DEBUG(LogModule::NETWORK, oss.str());
      this->outputBuffer_.retrieve(static_cast<size_t>(n));
      write_res.bytes_sent += n;

    } else if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::ostringstream oss;
        oss << "send would block, fd=" << this->fd_;
        LOG_DEBUG(LogModule::NETWORK, oss.str());
        return write_res;
      }
      std::ostringstream oss;
      oss << "send failed, fd=" << this->fd_ << ", errno=" << errno << ", err=" << strerror(errno);
      LOG_ERROR(LogModule::NETWORK, oss.str());
      this->setState(ConnState::Disconnected);
      write_res.ok = false;
      write_res.close = true;
      return write_res;
    } else {
      std::ostringstream oss;
      oss << "send returned 0, fd=" << this->fd_;
      LOG_WARN(LogModule::NETWORK, oss.str());
      this->setState(ConnState::Disconnected);
      write_res.ok = false;
      write_res.close = true;
      return write_res;
    }
  }

  if (this->canBeClosed()) {
    std::ostringstream oss;
    oss << "write buffer drained, closing disconnecting connection, fd=" << this->fd_;
    LOG_DEBUG(LogModule::NETWORK, oss.str());
    this->setState(ConnState::Disconnected);
    write_res.close = true;
    return write_res;
  }

  std::ostringstream oss;
  oss << "write buffer drained, fd=" << this->fd_;
  LOG_DEBUG(LogModule::NETWORK, oss.str());
  return write_res;
}

void Connection::setMessageCallback(MessageCallback cb) {
  this->on_message_ = std::move(cb);
}

void Connection::setWriteReadyCallback(
    std::function<void(const std::shared_ptr<Connection>&)> cb) {
  on_write_ready_ = std::move(cb);
}

bool Connection::sendPacket(const std::vector<char> &packet) {
  if (isDisconnected() || owner_loop_ == nullptr || packet.empty()) {
    return false;
  }
  auto weak = weak_from_this();
  if (weak.expired()) return false;
  ++pending_packets_;
  try {
    if (owner_loop_->tryQueueInLoop([weak, packet]() {
          auto conn = weak.lock();
          if (!conn) return;
          --conn->pending_packets_;
          if (conn->isDisconnected()) return;
          try {
            conn->outputBuffer_.append(packet.data(), packet.size());
          } catch (...) {
            conn->setState(ConnState::Disconnected);
          }
          if (conn->on_write_ready_) conn->on_write_ready_(conn);
        })) {
      return true;
    }
  } catch (...) {
    // Allocation failed before queue acceptance; the caller may roll back.
  }
  --pending_packets_;
  return false;
}

void Connection::send(const std::string &data) {
  sendPacket(std::vector<char>(data.begin(), data.end()));
}

void Connection::setResponseQueuedCallback(std::function<void(bool)> cb) {on_response_queued_=std::move(cb);}
bool Connection::sendResponsePacket(const std::vector<char>& packet,bool heartbeat) {
  if(!sendPacket(packet))return false;
  if(on_response_queued_)on_response_queued_(heartbeat);
  return true;
}
void Connection::setLogIdentity(const std::string& role,const std::string& worker) {
  std::lock_guard<std::mutex> lock(identity_mutex_);log_role_=role;log_worker_=worker;
}
std::string Connection::logContext() const {
  std::lock_guard<std::mutex> lock(identity_mutex_);
  return "connection_id="+std::to_string(connection_id_)+" fd="+std::to_string(fd_)+" peer="+peer_+" role="+log_role_+
      (log_worker_.empty()?std::string():" worker_id="+log_worker_);
}
bool Connection::isWorkerConnection() const {std::lock_guard<std::mutex> lock(identity_mutex_);return log_role_=="WORKER";}

bool Connection::wantWrite() const {
  return this->outputBuffer_.readableBytes() > 0;
}

Connection::ConnState Connection::state() const { return this->state_.load(); }

void Connection::setState(Connection::ConnState st) {
  if (this->state_.load() != st) {
    std::ostringstream oss;
    oss << "connection state change, fd=" << this->fd_ << ", from=" << static_cast<int>(this->state_.load())
         << ", to=" << static_cast<int>(st);
    LOG_DEBUG(LogModule::NETWORK, oss.str());
  }
  this->state_.store(st);
}

void Connection::shutdown() {
  if (this->state_ == ConnState::Connected) {
    std::ostringstream oss;
    oss << "connection enter disconnecting, fd=" << this->fd_;
    LOG_DEBUG(LogModule::NETWORK, oss.str());
    this->state_.store(ConnState::Disconnecting);
  }
}

bool Connection::shouldCloseAfterWrite() const {
  return this->state_ == ConnState::Disconnecting &&
         this->outputBuffer_.readableBytes() == 0;
}

bool Connection::isConnected() const {
  return this->state_ == Connection::ConnState::Connected;
}

bool Connection::isDisconnecting() const {
  return this->state_ == Connection::ConnState::Disconnecting;
}

bool Connection::isDisconnected() const {
  return this->state_ == Connection::ConnState::Disconnected;
}

void Connection::incPendingTasks() { ++this->pending_tasks_; }

void Connection::decPendingTasks() { --this->pending_tasks_; }

int Connection::enqueueProtocolMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(protocol_mutex_);
  constexpr size_t max_bytes = 4 * 1024 * 1024;
  if (message.size() > max_bytes - protocol_bytes_ || protocol_messages_.size() >= 512)
    return -1;
  protocol_messages_.push_back(message);
  protocol_bytes_ += message.size();
  if (protocol_active_) return 0;
  protocol_active_ = true;
  return 1;
}

std::optional<std::string> Connection::nextProtocolMessage() {
  std::lock_guard<std::mutex> lock(protocol_mutex_);
  if (protocol_messages_.empty()) {
    protocol_active_ = false;
    return std::nullopt;
  }
  std::string message = std::move(protocol_messages_.front());
  protocol_messages_.pop_front();
  protocol_bytes_ -= message.size();
  return message;
}

size_t Connection::abandonProtocolMessages() {
  std::lock_guard<std::mutex> lock(protocol_mutex_);
  const auto count = protocol_messages_.size();
  protocol_messages_.clear();
  protocol_bytes_ = 0;
  protocol_active_ = false;
  return count;
}

bool Connection::hasPendingTasks() const {
  return this->pending_tasks_.load() > 0;
}

bool Connection::canBeClosed() const {
  return this->state_ == ConnState::Disconnecting &&
         this->outputBuffer_.readableBytes() == 0 && !this->hasPendingTasks() &&
         pending_packets_.load() == 0;
}

void Connection::refreshActivity() {
  this->last_active_ms_.store(getSteadyClockMs());
}

uint64_t Connection::lastActiveMs() const {
  return this->last_active_ms_.load();
}
