#ifndef GCRP_PROTOCOL_ROUTER_H
#define GCRP_PROTOCOL_ROUTER_H

#include "task/TaskService.h"
#include "task/Task.h"
#include "task/TaskStatus.h"
#include "scheduler/Scheduler.h"
#include "worker/WorkerManager.h"
#include "protocol/ProtocolMessage.h"
#include "protocol/MessageType.h"
#include "protocol/dto/TaskDTO.h"
#include "protocol/dto/WorkerDTO.h"
#include "protocol/dto/Response.h"
#include "regression/RegressionService.h"
#include "log/RunLogArchive.h"
#include "protocol/GuiCatalog.h"
#include "protocol/TaskListQuery.h"

namespace GCRP {

class SchedulingDriver;
class AutomationService;

} // namespace GCRP

namespace GCRP::Protocol {

enum class ResponseCode {
    OK = 0,
    INVALID_REQUEST = 1,
    NOT_FOUND = 2,
    INVALID_STATE = 3,
    INTERNAL_ERROR = 4
};

class ProtocolRouter {
public:
    ProtocolRouter(
        TaskService& task_service,
        Scheduler& scheduler,
        WorkerManager& worker_manager,
        SchedulingDriver& scheduling_driver,
        AutomationService* automation = nullptr
    );

    ProtocolMessage handle(const ProtocolMessage& request);
    void holdWorker(const WorkerId& id) { scheduler_.holdWorker(id); }
    size_t pruneLogs(int days) { return log_archive_.pruneOlderThan(days); }

    ProtocolRouter(const ProtocolRouter&) = delete;
    ProtocolRouter& operator=(const ProtocolRouter&) = delete;

private:
    ResponseCode toErrorCode(TaskStatus status) const;
    ProtocolMessage buildResponse(
        const ProtocolMessage& request,
        ResponseCode code,
        const std::string& message = "",
        const nlohmann::json& data = nlohmann::json{}
    ) const;
    nlohmann::json taskToJson(const Task& task) const;

    TaskService& task_service_;
    Scheduler& scheduler_;
    WorkerManager& worker_manager_;
    SchedulingDriver& scheduling_driver_;
    RegressionService regression_service_;
    RunLogArchive log_archive_;
    GuiCatalog gui_catalog_;
    Api::TaskListQuery task_lists_;
    AutomationService* automation_;
};

} // namespace GCRP::Protocol

#endif // GCRP_PROTOCOL_ROUTER_H
