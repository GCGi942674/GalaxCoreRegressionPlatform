#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

struct RpcReply {
    bool ok{false};
    QString error;
    Json result;
    std::string instance;
};

class RpcClient : public QObject {
public:
    explicit RpcClient(QObject* parent = nullptr)
        : QObject(parent) {
    }

    void configure(QString host, quint16 port, int timeout);
    void request(const std::string& type, const Json& data, std::function<void(RpcReply)> callback);

private:
    QString host_;
    quint16 port_{0};
    int timeout_{5000};
    uint64_t sequence_{0};
};
