#ifndef GCRP_TASK_SERVICE_H
#define GCRP_TASK_SERVICE_H

#include "task/Task.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "task/TaskIdGenerator.h"
#include "scheduler/Scheduler.h"
#include <optional>
#include <string>
#include "nlohmann/json.hpp"

namespace GCRP {

struct CreateTaskRequest {
    TaskType type;
    std::string target;
    std::string revision;
    int priority{0};
    nlohmann::json payload = nlohmann::json::object();
    std::string submission_key;
    std::string timeout_mode{"fixed"};
};

class SchedulingDriver;

class TaskService {
public:
    explicit TaskService(Scheduler& scheduler, SchedulingDriver& scheduling_driver);

    std::optional<TaskId> createTask(const CreateTaskRequest& request);
    std::optional<Task> getTask(TaskId id) const;
    bool cancelTask(TaskId id);

    void setNextTaskId(TaskId next) {
        id_generator_.setNext(next);
    }

    TaskService(const TaskService&) = delete;
    TaskService& operator=(const TaskService&) = delete;

private:
    bool isPriorityValid(int priority) const;

    Scheduler& scheduler_;
    SchedulingDriver& scheduling_driver_;
    TaskIdGenerator id_generator_;
};

} // namespace GCRP

#endif // GCRP_TASK_SERVICE_H
