#include "Acceptor.h"
#include "Logging.h"
#include "Utils.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace GCRP;

Acceptor::Acceptor(EventLoop* loop, int port, std::string bind_ip)
    : loop_(loop)
    , port_(port)
    , bind_ip_(std::move(bind_ip))
    , listen_fd_(-1) {
}

Acceptor::~Acceptor() {
    if (listen_fd_ != -1) {
        close(this->listen_fd_);
        this->listen_fd_ = -1;
    }
}

bool Acceptor::startListen() {
    this->listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);

    if (this->listen_fd_ < 0) {
        std::ostringstream oss;
        oss << "socket failed, error = " << errno << ", err = " << strerror(errno);
        LOG_ERROR(LogModule::NETWORK, oss.str());
        return false;
    }

    int opt = 1;
    setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    setNonBlocking(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(this->port_);
    if (::inet_pton(AF_INET, bind_ip_.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR(LogModule::NETWORK, "invalid configured bind IP");
        close(this->listen_fd_);
        this->listen_fd_ = -1;
        return false;
    }

    if (bind(this->listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::ostringstream oss;
        oss << "bind failed, fd=" << this->listen_fd_ << ", port=" << this->port_
            << ", errno=" << errno << ", err=" << strerror(errno);
        LOG_ERROR(LogModule::NETWORK, oss.str());
        close(this->listen_fd_);
        this->listen_fd_ = -1;
        return false;
    }

    if (listen(this->listen_fd_, 128) < 0) {
        std::ostringstream oss;
        oss << "listen failed, fd=" << this->listen_fd_ << ", errno=" << errno
            << ", err=" << strerror(errno);
        LOG_ERROR(LogModule::NETWORK, oss.str());
        close(this->listen_fd_);
        this->listen_fd_ = -1;
        return false;
    }

    {
        std::ostringstream oss;
        oss << "listen started, fd=" << this->listen_fd_ << ", port=" << this->port_;
        LOG_INFO(LogModule::NETWORK, oss.str());
    }

    this->loop_->addFd(this->listen_fd_, EPOLLIN, [this](uint32_t event) {
        this->handleAccept();
    });
    return true;
}

void Acceptor::handleAccept() {
    while (true) {
        int client_fd = accept(this->listen_fd_, nullptr, nullptr);
        if (client_fd >= 0) {
            setNonBlocking(client_fd);
            std::ostringstream oss;
            oss << "accept new connection, client_fd=" << client_fd;
            LOG_DEBUG(LogModule::NETWORK, oss.str());
            if (this->callback_) {
                this->callback_(client_fd);
            } else {
                std::ostringstream oss;
                oss << "new connection callback not set, client_fd=" << client_fd;
                LOG_WARN(LogModule::NETWORK, oss.str());
                close(client_fd);
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            std::ostringstream oss;
            oss << "accept failed, errno=" << errno << ", err=" << strerror(errno);
            LOG_ERROR(LogModule::NETWORK, oss.str());
            break;
        }
    }
}

void Acceptor::stopListen() {
    if (this->listen_fd_ != -1) {
        this->loop_->removeFd(this->listen_fd_);
        close(this->listen_fd_);
        this->listen_fd_ = -1;
        LOG_INFO(LogModule::NETWORK, "Acceptor stop listening");
    }
}

void Acceptor::setNewConnectionCallback(ServerCallback callback) {
    this->callback_ = std::move(callback);
}
