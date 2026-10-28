#ifndef GCRP_TASK_DTO_H
#define GCRP_TASK_DTO_H

#include "../ProtocolError.h"
#include "task/Task.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "nlohmann/json.hpp"
#include <string>

namespace GCRP::Protocol::DTO {

struct SubmitTaskRequest {
    TaskType task_type{TaskType::UNKNOWN};
    std::string target;
    std::string revision;
    int priority{0};
    nlohmann::json payload = nlohmann::json::object();
    std::string submission_key;
    std::string timeout_mode{"fixed"};
};

struct QueryTaskRequest {
    TaskId task_id;
};

struct CancelTaskRequest {
    TaskId task_id;
};

struct TaskStartRequest {
    TaskId task_id;
    std::string worker_id;
    std::string run_id;
};

struct TaskResultRequest {
    TaskId task_id;
    std::string worker_id;
    std::string run_id;
    TaskStatus status;
    std::string message;
};

struct TaskAssignRequest {
    nlohmann::json timeout_decision = nullptr;
    TaskId task_id{0};
    std::string run_id;
    TaskType task_type{TaskType::UNKNOWN};
    std::string target;
    std::string revision;
    int priority{0};
    nlohmann::json payload = nlohmann::json::object();
};

// Conversion functions
bool fromJson(const nlohmann::json& j, SubmitTaskRequest& out);
nlohmann::json toJson(const SubmitTaskRequest& value);

bool fromJson(const nlohmann::json& j, QueryTaskRequest& out);
nlohmann::json toJson(const QueryTaskRequest& value);

bool fromJson(const nlohmann::json& j, CancelTaskRequest& out);
nlohmann::json toJson(const CancelTaskRequest& value);

bool fromJson(const nlohmann::json& j, TaskStartRequest& out);
nlohmann::json toJson(const TaskStartRequest& value);

bool fromJson(const nlohmann::json& j, TaskResultRequest& out, ProtocolError& error);
nlohmann::json toJson(const TaskResultRequest& value);

bool fromJson(const nlohmann::json& j, TaskAssignRequest& out);
nlohmann::json toJson(const TaskAssignRequest& value);

} // namespace GCRP::Protocol::DTO

#endif // GCRP_TASK_DTO_H
