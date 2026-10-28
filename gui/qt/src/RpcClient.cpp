#include "RpcClient.h"
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <memory>

void RpcClient::configure(QString host, quint16 port, int timeout) {
    host_ = std::move(host);
    port_ = port;
    timeout_ = timeout;
}

void RpcClient::request(
    const std::string& type, const Json& data, std::function<void(RpcReply)> callback) {
    const auto id = ++sequence_;
    auto socket = new QTcpSocket(this);
    socket->setReadBufferSize(1048580);
    auto timer = new QTimer(socket);
    timer->setSingleShot(true);

    struct State {
        QByteArray buffer;
        bool done{false};
    };

    auto state = std::make_shared<State>();
    auto finish = [socket, timer, state, callback](RpcReply reply) {
        if (state->done)
            return;
        state->done = true;
        timer->stop();
        socket->abort();
        socket->deleteLater();
        callback(std::move(reply));
    };
    std::string body;
    try {
        body = Json{{"version", 1}, {"type", type}, {"request_id", id}, {"data", data}}.dump();
    } catch (const std::exception& e) {
        finish({false, QString::fromUtf8(e.what()), {}, {}});
        return;
    }
    if (body.size() > 1048576) {
        finish({false, QStringLiteral("请求超过 1 MiB"), {}, {}});
        return;
    }
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(quint32(body.size()), reinterpret_cast<uchar*>(frame.data()));
    frame.append(body.data(), int(body.size()));
    connect(socket, &QTcpSocket::connected, this, [socket, frame, finish] {
        if (socket->write(frame) != frame.size())
            finish({false, QStringLiteral("发送失败；变更请求不会自动重试"), {}, {}});
    });
    connect(timer, &QTimer::timeout, this, [finish] {
        finish({false, QStringLiteral("请求超时；变更请求可能已被接收，请先查询"), {}, {}});
    });
    connect(socket,
        QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
        this,
        [socket, finish](QAbstractSocket::SocketError) {
            finish({false, socket->errorString(), {}, {}});
        });
    connect(socket, &QTcpSocket::readyRead, this, [socket, state, id, finish] {
        if (state->done)
            return;
        state->buffer.append(socket->readAll());
        if (state->buffer.size() < 4)
            return;
        auto size =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(state->buffer.constData()));
        if (!size || size > 1048576 || state->buffer.size() > int(size) + 4) {
            finish({false, QStringLiteral("响应帧长度无效"), {}, {}});
            return;
        }
        if (state->buffer.size() < int(size) + 4)
            return;
        try {
            auto message =
                Json::parse(state->buffer.constData() + 4, state->buffer.constData() + 4 + size);
            if (message.at("version") != 1 || message.at("type") != "response" ||
                message.at("request_id") != id || !message.at("data").is_object())
                throw std::runtime_error("invalid response envelope");
            const auto& answer = message.at("data");
            if (!answer.at("code").is_number_integer())
                throw std::runtime_error("invalid response code");
            auto instance = answer.value("server_instance_id", std::string{});
            if (answer.at("code") != 0) {
                finish({false,
                    QString::fromStdString(
                        answer.value("message", std::string("Server rejected request"))),
                    {},
                    instance});
                return;
            }
            finish({true, {}, answer.at("result"), instance});
        } catch (const std::exception& e) {
            finish({false, QStringLiteral("无效响应：") + QString::fromUtf8(e.what()), {}, {}});
        }
    });
    connect(socket, &QTcpSocket::disconnected, this, [finish, state] {
        if (!state->done)
            finish({false, QStringLiteral("连接关闭，响应不完整"), {}, {}});
    });
    timer->start(timeout_);
    socket->connectToHost(host_, port_);
}
