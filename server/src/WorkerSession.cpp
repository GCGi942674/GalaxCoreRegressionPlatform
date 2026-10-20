#include "worker/WorkerSession.h"
#include "scheduler/SchedulingDriver.h"
#include "protocol/dto/WorkerDTO.h"
#include "protocol/dto/Response.h"
#include "protocol/ProtocolVersion.h"
#include "protocol/ConnectionRole.h"
#include "protocol/Message_codec.h"
#include "Logging.h"

using namespace GCRP::Protocol;
using namespace GCRP::Protocol::DTO;

namespace GCRP {

WorkerSession::WorkerSession(EventLoop* loop, WorkerManager& worker_manager,
    WorkerConnectionRegistry& registry, ProtocolRouter& router, SchedulingDriver& scheduling_driver)
    : loop_(loop), worker_manager_(worker_manager), registry_(registry), router_(router), scheduling_driver_(scheduling_driver) {}

WorkerSession::~WorkerSession() = default;

std::optional<WorkerId> WorkerSession::getWorkerId() const { return worker_id_; }
std::shared_ptr<Connection> WorkerSession::getConnection() const { return connection_weak_.lock(); }
void WorkerSession::setConnection(std::shared_ptr<Connection> conn) { connection_weak_ = conn; }
bool WorkerSession::isRegistered() const { return worker_id_.has_value(); }

void WorkerSession::onMessage(const std::string& json_msg) {
    ProtocolMessage request;
    ProtocolError error;
    if (!ProtocolCodec::decode(json_msg, request, error)) {
        reply(0, ResponseCode::INVALID_REQUEST, "invalid protocol message");
        return;
    }
    try {
        handleRequest(request);
    } catch (const nlohmann::json::exception&) {
        reply(request.request_id, ResponseCode::INVALID_REQUEST, "invalid message fields");
    }
}

void WorkerSession::handleRequest(const ProtocolMessage& request) {
    // The host serializes session messages, registration and close operations.
    if (isRegistered() && (!binding_handle_ ||
        !registry_.isCurrent(*worker_id_, *binding_handle_))) {
        reply(request.request_id, ResponseCode::INVALID_STATE, "worker connection replaced");
        return;
    }

    if (request.type == MessageType::WORKER_REGISTER) {
        WorkerRegisterRequest dto;
        ProtocolError error;
        if (!fromJson(request.data, dto, error)) {
            reply(request.request_id, ResponseCode::INVALID_REQUEST, error.message);
            return;
        }
        // Validate identity before changing WorkerManager or the registry.
        if (worker_id_ && *worker_id_ != dto.worker_id) {
            reply(request.request_id, ResponseCode::INVALID_REQUEST, "worker identity change not allowed");
            return;
        }
        auto conn = getConnection();
        if (!conn || !conn->isConnected()) return;
        const bool recovering = request.data.contains("recovery_version");
        if (recovering && (request.data["recovery_version"] != 1 || dto.slots != 1)) {
            reply(request.request_id, ResponseCode::INVALID_REQUEST, "recovery v1 requires one slot");
            return;
        }
        if (recovering) router_.holdWorker(dto.worker_id);
        if (!worker_manager_.registerWorker(dto.worker_id, dto.hostname, dto.slots,
                                            dto.machine_id, dto.slot_id)) {
            reply(request.request_id, ResponseCode::INVALID_REQUEST, "invalid worker registration");
            return;
        }
        auto handle = registry_.bindWithHandle(dto.worker_id, conn);
        if (!handle) {
            reply(request.request_id, ResponseCode::INTERNAL_ERROR, "connection binding failed");
            return;
        }
        worker_id_ = dto.worker_id;
        conn->setLogIdentity("WORKER",dto.worker_id);
        worker_manager_.setControlSupported(dto.worker_id,dto.control_v1);
        recovery_protocol_ = recovering;
        binding_handle_ = *handle;
        LOG_DEBUG(LogModule::WORKER, "worker connection bound, " + conn->logContext());
        ProtocolMessage response;response.request_id=request.request_id;response.type=MessageType::RESPONSE;
        response.data=DTO::toJson(DTO::Response{0,"ok",{{"archive_lifecycle_v1",true}}});
        sendToConnection(encodeResponse(response));
        if (!recovering) scheduling_driver_.requestSchedule();
        return;
    }

    if (!isAllowed(ConnectionRole::WORKER, request.type)) {
        reply(request.request_id, ResponseCode::INVALID_REQUEST, "message not allowed on worker connection");
        return;
    }
    if (!isRegistered()) {
        reply(request.request_id, ResponseCode::INVALID_STATE, "worker not registered");
        return;
    }
    if (!validateWorkerIdentity(request)) {
        reply(request.request_id, ResponseCode::INVALID_REQUEST, "worker identity mismatch");
        return;
    }
    if (request.type == MessageType::WORKER_RECOVER && !recovery_protocol_) {
        reply(request.request_id, ResponseCode::INVALID_STATE, "register recovery_version first");
        return;
    }
    sendToConnection(encodeResponse(router_.handle(request)));
}

void WorkerSession::handleClose() {
    if (!worker_id_) return;
    if (binding_handle_) {
        const auto id = *worker_id_;
        // Old A closing after replacement B must affect neither B nor its liveness.
        registry_.unbind(id, *binding_handle_, [this, &id]() {
            worker_manager_.markOffline(id);
            LOG_INFO(LogModule::WORKER,"worker disconnected; marked offline, worker_id="+id);
        });
    }
    binding_handle_.reset();
    worker_id_.reset();
    connection_weak_.reset();
}

void WorkerSession::reply(uint64_t request_id, ResponseCode code, const std::string& message) {
    ProtocolMessage response;
    response.version = PROTOCOL_VERSION;
    response.type = MessageType::RESPONSE;
    response.request_id = request_id;
    Response dto;
    dto.code = static_cast<int>(code);
    dto.message = message;
    response.data = toJson(dto);
    sendToConnection(encodeResponse(response));
}

std::vector<char> WorkerSession::encodeResponse(const ProtocolMessage& response) {
    std::string json_text;
    ProtocolError error;
    if (!ProtocolCodec::encode(response, json_text, error)) {
        LOG_ERROR(LogModule::NETWORK, "response encoding failed: " + error.message);
        return {};
    }
    return MessageCodec::encode(json_text);
}

void WorkerSession::sendToConnection(const std::vector<char>& packet) {
    auto conn = getConnection();
    if (conn && !packet.empty() && !conn->sendResponsePacket(packet)) {
        LOG_WARN(LogModule::NETWORK, "worker response queue rejected, "+conn->logContext());
    }
}

bool WorkerSession::validateWorkerIdentity(const ProtocolMessage& msg) {
    return worker_id_ && msg.data.contains("worker_id") &&
        msg.data["worker_id"].is_string() &&
        msg.data["worker_id"].get<std::string>() == *worker_id_;
}

} // namespace GCRP
