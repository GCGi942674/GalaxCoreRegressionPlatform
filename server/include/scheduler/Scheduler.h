#ifndef GCRP_SCHEDULER_H
#define GCRP_SCHEDULER_H

#include "task/Task.h"
#include "task/TaskQuery.h"
#include "task/TaskStatus.h"
#include "worker/WorkerManager.h"
#include "worker/Worker.h"
#include <unordered_map>
#include <vector>
#include <optional>
#include <queue>
#include <mutex>
#include <cstdint>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <limits>

namespace GCRP {
class WorkerDispatcher;
class StateStore;

struct TaskAssignment {
    TaskId task_id;
    WorkerId worker_id;
};

struct BatchSnapshot {
    bool stop_requested{false};
    size_t total{0}, pending{0}, queued{0}, assigned{0}, running{0};
    size_t succeeded{0}, failed{0}, timeout{0}, cancelled{0};
    size_t cancelled_now{0};
};

class Scheduler {
public:
    explicit Scheduler(WorkerManager& worker_manager,
                       size_t max_running_tasks = std::numeric_limits<size_t>::max());
    void restoreFrom(StateStore& store);
    TaskId nextTaskId() const;
    void holdWorker(const WorkerId& worker_id);
    nlohmann::json recoverWorker(const WorkerId& worker_id, const std::string& state,
                                 TaskId task_id, const std::string& run_id, int max_retries);

    bool submitTask(Task task);
    std::optional<TaskId> submitTask(Task task, const std::string& submission_key,
                                     const std::string& request_json);
    std::optional<TaskId> findSubmittedTask(const std::string& submission_key,
                                            const std::string& request_json) const;
    std::vector<TaskAssignment> schedule();
    bool completeTask(TaskId task_id, TaskStatus final_status,
                      nlohmann::json execution_summary = nullptr);
    bool cancelTask(TaskId task_id);
    size_t deleteRecords(TaskId id, const std::string& batch_id);
    bool isBatchDeleted(const std::string& batch_id) const;
    size_t deletedCount(const std::string& batch_id) const;
    std::optional<BatchSnapshot> queryBatch(const std::string& batch_id) const;
    std::optional<BatchSnapshot> cancelBatch(const std::string& batch_id);
    bool isBatchStopped(const std::string& batch_id) const;

    bool markTaskStarted(TaskId task_id, const WorkerId& worker_id);
    bool rollbackAssignment(TaskId task_id);
    // nullopt: assignment was cancelled/replaced before the dispatch claim.
    // Cancellation/rollback returns false while the dispatcher is in flight.
    std::optional<bool> dispatchAssignment(const TaskAssignment&, WorkerDispatcher&);

    std::optional<Task> getTask(TaskId id, bool include_deleted = false) const;
    // Resolve a completed current or persisted historical run to its Worker.
    std::optional<WorkerId> archiveOwner(TaskId id, const std::string& run_id) const;
    std::vector<Task> getTasks() const;
    // The tasks and generation are captured under the same lock.
    std::pair<uint64_t, std::vector<Task>> taskSnapshot() const;
    uint64_t taskGeneration() const;
    nlohmann::json statusCounts() const;
    void publishDailyPlan(const nlohmann::json& manifest);
    nlohmann::json dailyPlan(const std::string& day) const;
    uint64_t visitTasks(const std::function<void(const Task&)>& visitor,const std::string& batch_id="",
                        const std::vector<std::string>& dates={}) const;
    std::optional<std::pair<uint64_t,std::vector<TaskGroupSnapshot>>> taskGroups(
        const std::string& target,const std::string& revision,const std::string& batch_id) const;
    uint64_t excludedRegressionCount() const;
    std::vector<Task> getTasksByStatus(TaskStatus status) const;

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

private:
    struct QueuedTaskInfo {
        TaskId task_id;
        int priority;
        uint64_t sequence;

        bool operator>(const QueuedTaskInfo& other) const {
            if (priority != other.priority) {
                return priority < other.priority;
            }
            return sequence > other.sequence;
        }
    };

    WorkerManager& worker_manager_;
    size_t max_running_tasks_;
    std::unordered_map<TaskId, Task> tasks_;
    struct TaskQueue : std::priority_queue<QueuedTaskInfo, std::vector<QueuedTaskInfo>, std::greater<QueuedTaskInfo>> {
        void reserve(size_t count) {
            if (this->c.capacity() < count)
                this->c.reserve(std::max(count, this->c.capacity() * 2));
        }
    };
    TaskQueue task_queue_;
    std::unordered_set<TaskId> dispatching_;
    std::unordered_set<std::string> stopped_batches_;
    std::unordered_map<std::string,nlohmann::json> daily_plans_;
    std::unordered_set<std::string> quarantined_workers_;
    std::unordered_set<std::string> reconciling_workers_;
    StateStore* store_{nullptr};
    uint64_t next_sequence_;
    uint64_t task_generation_{0};
    std::string last_selected_worker_;
    mutable std::mutex mutex_;

    bool isValidFinalStatus(TaskStatus status) const;
    std::string recoverLost(Task& task, int max_retries);
    void releaseRecoveryHold(const WorkerId& worker_id);

};

} // namespace GCRP

#endif // GCRP_SCHEDULER_H
