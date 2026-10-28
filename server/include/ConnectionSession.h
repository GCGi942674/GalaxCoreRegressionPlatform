#pragma once
#include "worker/WorkerSession.h"
#include "protocol/ConnectionRole.h"

namespace GCRP {
// Access is serialized by WorkerServer's session mutex, including registration/close.
class ConnectionSession {
public:
    ConnectionSession(const std::shared_ptr<Connection>& connection,
        WorkerManager&,
        WorkerConnectionRegistry&,
        Protocol::ProtocolRouter&,
        SchedulingDriver&);
    void onMessage(const std::string&);
    void handleClose();
    bool authorizeLog(const Protocol::ProtocolMessage&);

    Protocol::ConnectionRole role() const {
        return role_;
    }

private:
    void sendResponse(const Protocol::ProtocolMessage&);
    void reply(uint64_t request_id, Protocol::ResponseCode, const std::string&);
    std::weak_ptr<Connection> connection_;
    Protocol::ConnectionRole role_{Protocol::ConnectionRole::UNKNOWN};
    Protocol::ProtocolRouter& router_;
    WorkerSession worker_;
};
} // namespace GCRP
