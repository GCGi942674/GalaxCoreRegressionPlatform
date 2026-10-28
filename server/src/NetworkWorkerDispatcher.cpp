#include "worker/NetworkWorkerDispatcher.h"
#include "worker/WorkerConnectionRegistry.h"
#include "task/Task.h"
#include "task/TaskStatus.h"
#include "protocol/ProtocolMessage.h"
#include "protocol/MessageType.h"
#include "protocol/ProtocolVersion.h"
#include "protocol/ProtocolCodec.h"
#include "protocol/dto/TaskDTO.h"
#include "protocol/Message_codec.h"
#include "Logging.h"

namespace GCRP {

NetworkWorkerDispatcher::NetworkWorkerDispatcher(
    WorkerConnectionRegistry& registry, uint8_t protocol_version)
    : registry_(registry)
    , protocol_version_(protocol_version) {
}

Protocol::ProtocolMessage NetworkWorkerDispatcher::buildTaskAssignMessage(const Task& task) {
    Protocol::ProtocolMessage message;
    message.version = protocol_version_;
    message.type = Protocol::MessageType::TASK_ASSIGN;
    message.request_id = next_request_id_++;

    Protocol::DTO::TaskAssignRequest assign_dto;
    assign_dto.task_id = task.getId();
    assign_dto.run_id = task.getRunId();
    assign_dto.task_type = task.getType();
    assign_dto.target = task.getTarget();
    assign_dto.revision = task.getRevision();
    assign_dto.priority = task.getPriority();
    assign_dto.payload = task.getPayload();
    assign_dto.timeout_decision = task.getTimeoutDecision();

    message.data = Protocol::DTO::toJson(assign_dto);
    return message;
}

bool NetworkWorkerDispatcher::dispatch(const WorkerId& worker_id, const Task& task) {
    if (!registry_.hasConnection(worker_id)) {
        LOG_WARN(LogModule::NETWORK, "dispatch failed: no connection for worker=" + worker_id);
        return false;
    }

    auto connection = registry_.getConnection(worker_id);
    if (!connection || !connection->isConnected()) {
        LOG_WARN(LogModule::NETWORK, "dispatch failed: expired connection for worker=" + worker_id);
        return false;
    }

    Protocol::ProtocolMessage message = buildTaskAssignMessage(task);

    std::string serialized;
    try {
        Protocol::ProtocolError error;
        if (!Protocol::ProtocolCodec::encode(message, serialized, error))
            return false;
    } catch (const std::exception& e) {
        LOG_ERROR(LogModule::NETWORK,
            "dispatch failed: serialization error for task=" + std::to_string(task.getId()) +
                " error=" + e.what());
        return false;
    }

    try {
        std::vector<char> framed = MessageCodec::encode(serialized);
        // No throwing work after acceptance: a queued message cannot be rolled back.
        return connection->sendPacket(framed);
    } catch (const std::exception& e) {
        LOG_ERROR(LogModule::NETWORK,
            "dispatch failed: send error for task=" + std::to_string(task.getId()) +
                " worker=" + worker_id + " error=" + e.what());
        return false;
    }
}

} // namespace GCRP
