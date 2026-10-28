#ifndef GCRP_NETWORK_WORKER_DISPATCHER_H
#define GCRP_NETWORK_WORKER_DISPATCHER_H

#include "worker/WorkerDispatcher.h"
#include "worker/WorkerConnectionRegistry.h"
#include "task/Task.h"
#include "protocol/ProtocolMessage.h"
#include "protocol/dto/TaskDTO.h"
#include <memory>
#include <atomic>
#include <string>

namespace GCRP {

class NetworkWorkerDispatcher : public WorkerDispatcher {
public:
    explicit NetworkWorkerDispatcher(
        WorkerConnectionRegistry& registry, uint8_t protocol_version = 1);

    ~NetworkWorkerDispatcher() override = default;

    bool dispatch(const WorkerId& worker_id, const Task& task) override;

    NetworkWorkerDispatcher(const NetworkWorkerDispatcher&) = delete;
    NetworkWorkerDispatcher& operator=(const NetworkWorkerDispatcher&) = delete;

private:
    Protocol::ProtocolMessage buildTaskAssignMessage(const Task& task);

    WorkerConnectionRegistry& registry_;
    uint8_t protocol_version_;
    std::atomic<uint64_t> next_request_id_{1};
};

} // namespace GCRP

#endif // GCRP_NETWORK_WORKER_DISPATCHER_H
