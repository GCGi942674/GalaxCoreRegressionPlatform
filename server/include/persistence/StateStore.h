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
#include <set>
#include "persistence/QueryMetrics.h"

struct sqlite3;

namespace GCRP {

struct PersistedTask {
    Task task;
    uint64_t queue_sequence;
};

struct HistorySnapshot {
    uint64_t generation{0}, excluded{0};
    std::vector<Task> tasks;
    std::set<std::string> templates;
};

class StateStore {
public:
    explicit StateStore(const std::string& path, int busy_timeout_ms = 5000);
    ~StateStore();
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    std::vector<PersistedTask> loadTasks(TaskId id = 0,
        bool active_only = false,
        const std::string& batch_id = "",
        bool include_deleted = false) const;
    std::pair<TaskId, uint64_t> highWaterMarks() const;
    nlohmann::json statusCounts() const;
    // A separate read-only connection and a WAL read transaction. Never takes
    // the Scheduler lock or the writer connection mutex.
    HistorySnapshot historySnapshot(const std::string& batch = "",
        const std::vector<std::string>& dates = {},
        const std::string& stage = "",
        const std::string& target = "",
        const std::vector<std::string>& revisions = {},
        bool scheduled_only = false,
        size_t max_rows = 200000) const;
    uint64_t historyVersion(const std::vector<std::string>& dates = {}) const;
    nlohmann::json regressionDates(const std::string& stage, const std::string& before) const;

    nlohmann::json historyStats() const {
        auto result = history_metrics_.json();
        result["version_checks"] = version_metrics_.json();
        return result;
    }

    nlohmann::json runtimeSnapshot(int days) const;
    void pruneRuntimeSamples(int days);
    void clearTimeoutGuard(const std::string& key);
    void publishDailyPlan(const nlohmann::json& manifest);
    nlohmann::json dailyPlan(const std::string& day) const;
    bool hasBatch(const std::string& id) const;
    bool batchDeleted(const std::string& id) const;
    size_t deletedCount(const std::string& batch_id) const;
    // Logical deletion preserves run ownership, submission keys and high-water marks.
    size_t deleteRecords(TaskId id, const std::string& batch_id);
    void visitTasks(const std::function<void(Task&&, uint64_t)>& visitor,
        TaskId id = 0,
        bool active_only = false,
        const std::string& batch_id = "",
        const std::vector<std::string>& dates = {},
        bool include_deleted = false) const;
    std::vector<TaskGroupSnapshot> taskGroups(
        const std::string& target, const std::string& revision, const std::string& batch_id) const;
    uint64_t excludedRegressionCount() const;
    std::vector<std::string> loadStoppedBatches() const;
    nlohmann::json loadWorkerControls() const;
    void saveWorkerControls(const nlohmann::json&);
    nlohmann::json loadAutomation() const;
    void saveAutomation(const nlohmann::json&);
    nlohmann::json loadArtifactProtection() const;
    void saveArtifactProtection(const nlohmann::json&);
    bool dailySubmissionComplete(const nlohmann::json& manifest) const;
    std::optional<std::pair<TaskId, std::string>> findSubmission(const std::string& key) const;
    std::optional<std::string> recoveryDecision(
        const std::string& run_id, TaskId task_id, const std::string& worker_id) const;
    // Only completed attempts may accept or expose archived evidence.
    std::optional<std::string> archiveOwner(TaskId task_id, const std::string& run_id) const;
    nlohmann::json archiveExportInfo(TaskId task_id, const std::string& run_id) const;
    // All task and batch changes are one durable transaction. Throws on failure.
    void save(const std::vector<Task>& tasks,
        const std::optional<std::string>& stopped_batch = std::nullopt,
        const std::optional<std::string>& run_status_override = std::nullopt,
        const std::optional<std::pair<std::string, std::string>>& submission = std::nullopt,
        const std::optional<std::pair<std::string, std::string>>& recovery = std::nullopt);

private:
    mutable std::mutex mutex_;
    sqlite3* db_{nullptr};
    sqlite3* history_db_{nullptr};
    mutable std::mutex history_mutex_;
    mutable QueryMetrics history_metrics_;
    mutable QueryMetrics version_metrics_;
    void bumpHistory(const std::set<std::string>& dates);
    int lock_fd_{-1};
    void exec(const char* sql) const;
};

} // namespace GCRP
#endif
