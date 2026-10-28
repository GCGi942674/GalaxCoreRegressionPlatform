#pragma once
#include "protocol/MessageType.h"

namespace GCRP::Protocol {
enum class ConnectionRole { UNKNOWN, CLIENT, WORKER };

constexpr bool isClientRequest(MessageType type) {
    return type == MessageType::SUBMIT_TASK || type == MessageType::QUERY_TASK ||
           type == MessageType::QUERY_STATUS || type == MessageType::QUERY_TASKS ||
           type == MessageType::QUERY_WORKERS || type == MessageType::QUERY_CAPABILITIES ||
           type == MessageType::QUERY_CATALOG || type == MessageType::CONTROL_WORKERS ||
           type == MessageType::RECOVER_OFFLINE_WORKERS || type == MessageType::DELETE_TASK ||
           type == MessageType::DELETE_BATCH || type == MessageType::CANCEL_TASK ||
           type == MessageType::QUERY_BATCH || type == MessageType::PUBLISH_DAILY_PLAN ||
           type == MessageType::QUERY_DAILY_PLAN || type == MessageType::QUERY_AUTOMATION ||
           type == MessageType::UPDATE_AUTOMATION || type == MessageType::CANCEL_BATCH ||
           type == MessageType::QUERY_REGRESSION || type == MessageType::QUERY_PROGRESS ||
           type == MessageType::QUERY_ROLLBACK || type == MessageType::QUERY_LOGS ||
           type == MessageType::READ_LOG;
}

constexpr bool isWorkerRequest(MessageType type) {
    return type == MessageType::WORKER_REGISTER || type == MessageType::WORKER_HEARTBEAT ||
           type == MessageType::WORKER_RECOVER || type == MessageType::WORKER_DRAIN ||
           type == MessageType::TASK_START || type == MessageType::TASK_RESULT ||
           type == MessageType::LOG_APPEND || type == MessageType::LOG_STATE;
}

// Single inbound permission policy. Roles are protocol separation, not authentication.
constexpr bool isAllowed(ConnectionRole role, MessageType type) {
    switch (role) {
        case ConnectionRole::UNKNOWN:
            return isClientRequest(type) || type == MessageType::WORKER_REGISTER;
        case ConnectionRole::CLIENT:
            return isClientRequest(type);
        case ConnectionRole::WORKER:
            return isWorkerRequest(type);
    }
    return false;
}
} // namespace GCRP::Protocol
