#ifndef GCRP_WORKER_SESSION_HANDLER_H
#define GCRP_WORKER_SESSION_HANDLER_H

#include "Message.h"
#include "worker/WorkerManager.h"
#include "worker/WorkerConnectionRegistry.h"
#include "worker/NetworkWorkerDispatcher.h"
#include "protocol/ProtocolRouter.h"
#include "scheduler/SchedulingCoordinator.h"
#include "task/TaskService.h"
#include "scheduler/Scheduler.h"
#include "EventLoop.h"
#include <memory>

namespace GCRP {

class WorkerSessionHandler : public Message {
public:
    struct Config {
        uint64_t idle_timeout_ms{60000};
    };

    explicit WorkerSessionHandler(WorkerManager& worker_manager,
        WorkerConnectionRegistry& registry,
        Scheduler& scheduler,
        TaskService& task_service,
        SchedulingCoordinator& coordinator,
        Protocol::ProtocolRouter& router);

    ~WorkerSessionHandler() override;

    std::string onMessage(const std::string& msg) override;

    void setEventLoop(EventLoop* loop) {
        loop_ = loop;
    }

private:
    EventLoop* loop_{nullptr};

    WorkerManager& worker_manager_;
    WorkerConnectionRegistry& registry_;
    NetworkWorkerDispatcher dispatcher_;
    Scheduler& scheduler_;
    TaskService& task_service_;
    SchedulingCoordinator& coordinator_;
    Protocol::ProtocolRouter& router_;
};

} // namespace GCRP

#endif // GCRP_WORKER_SESSION_HANDLER_H