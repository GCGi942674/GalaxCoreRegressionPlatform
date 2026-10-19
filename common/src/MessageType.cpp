#include "protocol/MessageType.h"
#include <unordered_map>

namespace GCRP::Protocol {

std::string toString(MessageType type) {
    switch (type) {
        case MessageType::SUBMIT_TASK:    return "submit_task";
        case MessageType::QUERY_TASK:     return "query_task";
        case MessageType::QUERY_STATUS:   return "query_status";
        case MessageType::QUERY_TASKS: return "query_tasks";
        case MessageType::QUERY_WORKERS: return "query_workers";
        case MessageType::QUERY_CAPABILITIES: return "query_capabilities";
        case MessageType::QUERY_CATALOG: return "query_catalog";
        case MessageType::CONTROL_WORKERS: return "control_workers";
        case MessageType::CANCEL_TASK:    return "cancel_task";
        case MessageType::DELETE_TASK: return "delete_task";
        case MessageType::DELETE_BATCH: return "delete_batch";
        case MessageType::QUERY_BATCH:    return "query_batch";
        case MessageType::PUBLISH_DAILY_PLAN: return "publish_daily_plan";
        case MessageType::QUERY_DAILY_PLAN: return "query_daily_plan";
        case MessageType::CANCEL_BATCH:   return "cancel_batch";
        case MessageType::QUERY_REGRESSION: return "query_regression";
        case MessageType::QUERY_PROGRESS: return "query_progress";
        case MessageType::QUERY_ROLLBACK: return "query_rollback";
        case MessageType::QUERY_LOGS: return "query_logs";
        case MessageType::READ_LOG: return "read_log";
        case MessageType::WORKER_REGISTER: return "worker_register";
        case MessageType::WORKER_HEARTBEAT: return "worker_heartbeat";
        case MessageType::WORKER_RECOVER: return "worker_recover";
        case MessageType::WORKER_DRAIN: return "worker_drain";
        case MessageType::TASK_ASSIGN:    return "task_assign";
        case MessageType::TASK_START:     return "task_start";
        case MessageType::TASK_RESULT:    return "task_result";
        case MessageType::LOG_APPEND:     return "log_append";
        case MessageType::LOG_STATE: return "log_state";
        case MessageType::RESPONSE:       return "response";
        default:                          return "unknown";
    }
}

MessageType messageTypeFromString(const std::string& value) {
    static const std::unordered_map<std::string, MessageType> type_map = {
        {"submit_task", MessageType::SUBMIT_TASK},
        {"query_task", MessageType::QUERY_TASK},
        {"query_status", MessageType::QUERY_STATUS},
        {"query_tasks", MessageType::QUERY_TASKS},
        {"query_workers", MessageType::QUERY_WORKERS},
        {"query_capabilities", MessageType::QUERY_CAPABILITIES},
        {"query_catalog", MessageType::QUERY_CATALOG},
        {"control_workers", MessageType::CONTROL_WORKERS},
        {"cancel_task", MessageType::CANCEL_TASK},
        {"delete_task", MessageType::DELETE_TASK},
        {"delete_batch", MessageType::DELETE_BATCH},
        {"query_batch", MessageType::QUERY_BATCH},
        {"publish_daily_plan", MessageType::PUBLISH_DAILY_PLAN},
        {"query_daily_plan", MessageType::QUERY_DAILY_PLAN},
        {"cancel_batch", MessageType::CANCEL_BATCH},
        {"query_regression", MessageType::QUERY_REGRESSION},
        {"query_progress", MessageType::QUERY_PROGRESS},
        {"query_rollback", MessageType::QUERY_ROLLBACK},
        {"query_logs", MessageType::QUERY_LOGS},
        {"read_log", MessageType::READ_LOG},
        {"worker_register", MessageType::WORKER_REGISTER},
        {"worker_heartbeat", MessageType::WORKER_HEARTBEAT},
        {"worker_recover", MessageType::WORKER_RECOVER},
        {"worker_drain", MessageType::WORKER_DRAIN},
        {"task_assign", MessageType::TASK_ASSIGN},
        {"task_start", MessageType::TASK_START},
        {"task_result", MessageType::TASK_RESULT},
        {"log_append", MessageType::LOG_APPEND},
        {"log_state", MessageType::LOG_STATE},
        {"response", MessageType::RESPONSE}
    };

    auto it = type_map.find(value);
    if (it != type_map.end()) {
        return it->second;
    }
    return MessageType::UNKNOWN;
}

} // namespace GCRP::Protocol
