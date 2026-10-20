#ifndef GCRP_MESSAGE_TYPE_H
#define GCRP_MESSAGE_TYPE_H

#include <string>

namespace GCRP::Protocol {

enum class MessageType {
    SUBMIT_TASK,
    QUERY_TASK,
    QUERY_STATUS,
    QUERY_TASKS,
    QUERY_WORKERS,
    QUERY_CAPABILITIES,
    QUERY_CATALOG,
    CONTROL_WORKERS,
    RECOVER_OFFLINE_WORKERS,
    CANCEL_TASK,
    DELETE_TASK,
    DELETE_BATCH,
    QUERY_BATCH,
    PUBLISH_DAILY_PLAN,
    QUERY_DAILY_PLAN,
    QUERY_AUTOMATION,
    UPDATE_AUTOMATION,
    CANCEL_BATCH,
    QUERY_REGRESSION,
    QUERY_PROGRESS,
    QUERY_ROLLBACK,
    QUERY_LOGS,
    READ_LOG,

    WORKER_REGISTER,
    WORKER_HEARTBEAT,
    WORKER_RECOVER,
    WORKER_DRAIN,

    TASK_ASSIGN,
    TASK_START,
    TASK_RESULT,
    LOG_APPEND,
    LOG_STATE,

    RESPONSE,

    UNKNOWN
};

std::string toString(MessageType type);
MessageType messageTypeFromString(const std::string& value);

} // namespace GCRP::Protocol

#endif // GCRP_MESSAGE_TYPE_H
