#ifndef GCRP_WORKER_SESSION_H
#define GCRP_WORKER_SESSION_H

#include "worker/WorkerManager.h"
#include "worker/WorkerConnectionRegistry.h"
#include "protocol/ProtocolRouter.h"
#include "protocol/ProtocolMessage.h"
#include "protocol/ProtocolError.h"
#include "protocol/ProtocolCodec.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Logging.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

namespace GCRP {

class SchedulingDriver;

class WorkerSession {
public:
    explicit WorkerSession(EventLoop* loop,
        WorkerManager& worker_manager,
        WorkerConnectionRegistry& registry,
        Protocol::ProtocolRouter& router,
        SchedulingDriver& scheduling_driver);

    ~WorkerSession();

    std::optional<WorkerId> getWorkerId() const;
    std::shared_ptr<Connection> getConnection() const;
    void setConnection(std::shared_ptr<Connection> conn);

    void onMessage(const std::string& json_msg);
    void handleClose();

    bool isRegistered() const;

    bool permitsLogUpload(const Protocol::ProtocolMessage& request) {
        return worker_id_ && binding_handle_ &&
               registry_.isCurrent(*worker_id_, *binding_handle_) &&
               validateWorkerIdentity(request);
    }

private:
    void handleRequest(const Protocol::ProtocolMessage& request);
    void reply(uint64_t request_id, Protocol::ResponseCode code, const std::string& message);
    std::vector<char> encodeResponse(const Protocol::ProtocolMessage& response);
    void sendToConnection(const std::vector<char>& packet);

    bool validateWorkerIdentity(const Protocol::ProtocolMessage& msg);

    EventLoop* loop_;
    std::weak_ptr<Connection> connection_weak_;

    std::optional<WorkerId> worker_id_;
    bool recovery_protocol_{false};
    std::optional<WorkerConnectionRegistry::HandleId> binding_handle_;

    WorkerManager& worker_manager_;
    WorkerConnectionRegistry& registry_;
    Protocol::ProtocolRouter& router_;
    SchedulingDriver& scheduling_driver_;

    static constexpr uint32_t kProtocolVersion = 1;
};

} // namespace GCRP

#endif // GCRP_WORKER_SESSION_H
