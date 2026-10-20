#ifndef GCRP_WORKER_SERVER_H
#define GCRP_WORKER_SERVER_H

#include "EchoServer.h"
#include "WorkerSessionHandler.h"
#include "worker/WorkerManager.h"
#include "worker/WorkerConnectionRegistry.h"
#include "worker/NetworkWorkerDispatcher.h"
#include "scheduler/Scheduler.h"
#include "task/TaskService.h"
#include "EventLoop.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <deque>
#include <functional>
#include "worker/WorkerSession.h"
#include "ConnectionSession.h"
#include "persistence/StateStore.h"

namespace GCRP {

class WorkerServer {
public:
    explicit WorkerServer(bool persistent = false);

    ~WorkerServer();
    // Call before transport.run(); this object must outlive the transport.
    void attach(EchoServer& transport);
    SchedulingResult runOnce();

    WorkerManager& getWorkerManager() { return *worker_manager_ptr_; }
    WorkerConnectionRegistry& getWorkerConnectionRegistry() { return *registry_ptr_; }
    Scheduler& getScheduler() { return *scheduler_ptr_; }
    TaskService& getTaskService() { return *task_service_ptr_; }
    Protocol::ProtocolRouter& getRouter() { return *router_ptr_; }

private:
    std::unique_ptr<StateStore> store_ptr_;
    std::unique_ptr<WorkerManager> worker_manager_ptr_;
    std::unique_ptr<WorkerConnectionRegistry> registry_ptr_;
    std::unique_ptr<NetworkWorkerDispatcher> dispatcher_ptr_;
    std::unique_ptr<Scheduler> scheduler_ptr_;
    std::unique_ptr<SchedulingCoordinator> coordinator_ptr_;
    std::unique_ptr<SchedulingDriver> scheduling_driver_ptr_;
    std::unique_ptr<TaskService> task_service_ptr_;
    std::unique_ptr<Protocol::ProtocolRouter> router_ptr_;
    std::mutex session_mutex_;
    std::unordered_map<Connection*, std::unique_ptr<ConnectionSession>> sessions_;
    std::thread liveness_thread_;
    std::mutex liveness_mutex_;
    std::condition_variable liveness_cv_;
    bool stopping_{false};
    std::vector<std::thread> log_threads_;
    std::mutex log_mutex_;
    std::condition_variable log_cv_;
    std::deque<std::function<void()>> log_jobs_;
    bool log_stopping_{false};
    std::vector<std::thread> query_threads_;
    std::mutex query_mutex_;
    std::condition_variable query_cv_;
    std::deque<std::function<void()>> query_jobs_;
    bool query_stopping_{false};
};

} // namespace GCRP

#endif // GCRP_WORKER_SERVER_H
