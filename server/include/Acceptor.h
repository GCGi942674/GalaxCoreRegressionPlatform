#ifndef ACCEPTOR_H_
#define ACCEPTOR_H_

#include "EventLoop.h"
#include <functional>
#include <string>

class Acceptor {
public:
    using ServerCallback = std::function<void(int)>;

public:
    Acceptor(EventLoop* loop, int port, std::string bind_ip = "0.0.0.0");
    ~Acceptor();

    bool startListen();
    void handleAccept();
    void stopListen();

    void setNewConnectionCallback(ServerCallback callback);

private:
    int listen_fd_;
    int port_;
    std::string bind_ip_;
    EventLoop* loop_;
    ServerCallback callback_;
};

#endif
