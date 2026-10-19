#include "ConnectionSession.h"
#include "protocol/Message_codec.h"
#include "protocol/dto/Response.h"

namespace GCRP {
using namespace Protocol;
ConnectionSession::ConnectionSession(const std::shared_ptr<Connection>& connection,
    WorkerManager& workers, WorkerConnectionRegistry& registry, ProtocolRouter& router,
    SchedulingDriver& driver)
    : connection_(connection), router_(router),
      worker_(connection->ownerLoop(), workers, registry, router, driver) {
    worker_.setConnection(connection);
}

void ConnectionSession::onMessage(const std::string& text) {
    ProtocolMessage request;
    ProtocolError error;
    if (!ProtocolCodec::decode(text, request, error)) {
        reply(0, ResponseCode::INVALID_REQUEST, "invalid protocol message");
        return;
    }
    if (!isAllowed(role_, request.type)) {
        reply(request.request_id, ResponseCode::INVALID_REQUEST,
              "message is not allowed for this connection role");
        return;
    }
    if (isWorkerRequest(request.type)) {
        worker_.onMessage(text);
        // Failed registrations do not establish a role.
        if (worker_.isRegistered()) role_ = ConnectionRole::WORKER;
        return;
    }
    // A valid envelope with a client request type establishes CLIENT even if its
    // business payload is rejected. No role switching during this connection.
    role_ = ConnectionRole::CLIENT;
    try {
        sendResponse(router_.handle(request));
    } catch (const nlohmann::json::exception&) {
        reply(request.request_id, ResponseCode::INVALID_REQUEST, "invalid message fields");
    } catch (const std::exception&) {
        reply(request.request_id, ResponseCode::INTERNAL_ERROR, "request processing failed");
    }
}

void ConnectionSession::handleClose() {
    // No-op for clients; also cleans up partially completed worker registration.
    worker_.handleClose();
}

bool ConnectionSession::authorizeLog(const ProtocolMessage& request) {
    if(request.type==MessageType::LOG_APPEND||request.type==MessageType::LOG_STATE)
        return role_==ConnectionRole::WORKER&&worker_.permitsLogUpload(request);
    if(request.type!=MessageType::QUERY_LOGS&&request.type!=MessageType::READ_LOG&&request.type!=MessageType::QUERY_DAILY_PLAN)return false;
    if(role_==ConnectionRole::WORKER)return false;
    role_=ConnectionRole::CLIENT;return true;
}

void ConnectionSession::reply(uint64_t id, ResponseCode code, const std::string& message) {
    ProtocolMessage response;
    response.type = MessageType::RESPONSE;
    response.request_id = id;
    response.data = DTO::toJson(DTO::Response{static_cast<int>(code), message, {}});
    sendResponse(response);
}

void ConnectionSession::sendResponse(const ProtocolMessage& response) {
    std::string text;
    ProtocolError error;
    if (!ProtocolCodec::encode(response, text, error))
        throw std::runtime_error("response serialization failed");
    if (text.size() > MessageCodec::kMaxBodyLenght) {
        ProtocolMessage fallback;
        fallback.type = MessageType::RESPONSE;
        fallback.request_id = response.request_id;
        fallback.data = DTO::toJson(DTO::Response{
            static_cast<int>(ResponseCode::INTERNAL_ERROR), "response exceeds frame limit", {}});
        if (!ProtocolCodec::encode(fallback, text, error))
            throw std::runtime_error("fallback serialization failed");
    }
    auto connection = connection_.lock();
    if (connection && !connection->sendPacket(MessageCodec::encode(text)))
        LOG_WARN(LogModule::NETWORK, "client response queue rejected");
}
} // namespace GCRP
