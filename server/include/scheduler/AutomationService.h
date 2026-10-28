#pragma once
#include "nlohmann/json.hpp"
#include "protocol/GuiCatalog.h"
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <ctime>

namespace GCRP {
class StateStore;
class Scheduler;
class TaskService;

// One Server-owned plan runner; durable snapshots and submission keys survive restart.
class AutomationService {
public:
    AutomationService(StateStore*, Scheduler&, TaskService&);
    ~AutomationService();
    void start();
    void stop();
    nlohmann::json query() const;
    nlohmann::json update(const nlohmann::json&);
    void publishDaily(const nlohmann::json&, bool submission_complete = false);
    void tick(std::time_t now = std::time(nullptr));

private:
    StateStore* store_;
    Scheduler& scheduler_;
    TaskService& tasks_;
    GuiCatalog catalog_;
    nlohmann::json state_;
    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::string runner_error_;
    bool progress_dirty_{false};
    void commit(nlohmann::json);
    nlohmann::json viewLocked() const;
    nlohmann::json snapshot(const nlohmann::json&, const std::string&);
};
} // namespace GCRP
