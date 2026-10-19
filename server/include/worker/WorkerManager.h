#ifndef GCRP_WORKER_MANAGER_H
#define GCRP_WORKER_MANAGER_H

#include "worker/Worker.h"
#include "worker/WorkerStatus.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>

namespace GCRP {
class StateStore;

class WorkerManager {
public:
    bool registerWorker(
        const WorkerId& worker_id,
        const std::string& hostname,
        uint32_t slots,
        const std::string& machine_id = "",
        const std::string& slot_id = ""
    );

    bool heartbeat(const WorkerId& worker_id);
    bool markOffline(const WorkerId& worker_id);
    bool markDraining(const WorkerId& worker_id);
    void setControlSupported(const WorkerId& id, bool supported);
    std::optional<nlohmann::json> control(bool machine, const std::string& target,
        const std::string& action, const std::string& operation_id, const std::string& scope);
    nlohmann::json managementHeartbeat(const WorkerId&, const nlohmann::json& ack);

    bool acquireSlot(const WorkerId& worker_id);
    bool releaseSlot(const WorkerId& worker_id);

    std::optional<Worker> getWorker(const WorkerId& id) const;
    std::vector<Worker> getWorkers() const;
    std::pair<uint64_t, std::vector<Worker>> snapshot() const;
    std::vector<Worker> getOnlineWorkers() const;
    std::vector<Worker> getAvailableWorkers() const;

    void checkTimeouts(std::chrono::seconds timeout);

    explicit WorkerManager(StateStore* store = nullptr);
    ~WorkerManager() = default;

    WorkerManager(const WorkerManager&) = delete;
    WorkerManager& operator=(const WorkerManager&) = delete;

private:
    // Recovery holds Scheduler then WorkerManager locks, including the durable
    // decision, so registration/heartbeats cannot race the OFFLINE check.
    friend class Scheduler;
    mutable std::mutex mutex_;
    uint64_t generation_{0};
    std::unordered_map<WorkerId, std::unique_ptr<Worker>> workers_;
    StateStore* store_;
    nlohmann::json controls_;
    std::shared_ptr<const WorkerControl> controlFor(const nlohmann::json&, const WorkerId&, const std::string&) const;
    void commitControls(nlohmann::json next);
    bool releaseSlotLocked(const WorkerId& worker_id); // caller holds mutex_
};

} // namespace GCRP

#endif // GCRP_WORKER_MANAGER_H
