#ifndef GCRP_STATE_STORE_H
#define GCRP_STATE_STORE_H

#include "task/Task.h"
#include "task/TaskQuery.h"
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

struct sqlite3;

namespace GCRP {

struct PersistedTask {
    Task task;
    uint64_t queue_sequence;
};

class StateStore {
public:
    explicit StateStore(const std::string& path, int busy_timeout_ms = 5000);
    ~StateStore();
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    std::vector<PersistedTask> loadTasks(TaskId id = 0, bool active_only = false,
                                         const std::string& batch_id = "") const;
    std::pair<TaskId,uint64_t> highWaterMarks() const;
    nlohmann::json statusCounts() const;
    void publishDailyPlan(const nlohmann::json& manifest);
    nlohmann::json dailyPlan(const std::string& day) const;
    bool hasBatch(const std::string& id) const;
    void visitTasks(const std::function<void(Task&&,uint64_t)>& visitor,TaskId id=0,
                    bool active_only=false,const std::string& batch_id="",
                    const std::vector<std::string>& dates={}) const;
    std::vector<TaskGroupSnapshot> taskGroups(const std::string& target,const std::string& revision,
                                             const std::string& batch_id) const;
    uint64_t excludedRegressionCount() const;
    std::vector<std::string> loadStoppedBatches() const;
    nlohmann::json loadWorkerControls() const;
    void saveWorkerControls(const nlohmann::json&);
    std::optional<std::pair<TaskId, std::string>> findSubmission(const std::string& key) const;
    std::optional<std::string> recoveryDecision(const std::string& run_id,
                                               TaskId task_id, const std::string& worker_id) const;
    // Only completed attempts may accept or expose archived evidence.
    std::optional<std::string> archiveOwner(TaskId task_id, const std::string& run_id) const;
    // All task and batch changes are one durable transaction. Throws on failure.
    void save(const std::vector<Task>& tasks,
              const std::optional<std::string>& stopped_batch = std::nullopt,
              const std::optional<std::string>& run_status_override = std::nullopt,
              const std::optional<std::pair<std::string, std::string>>& submission = std::nullopt,
              const std::optional<std::pair<std::string, std::string>>& recovery = std::nullopt);

private:
    mutable std::mutex mutex_;
    sqlite3* db_{nullptr};
    int lock_fd_{-1};
    void exec(const char* sql) const;
};

} // namespace GCRP
#endif
