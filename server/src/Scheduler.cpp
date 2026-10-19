#include "scheduler/Scheduler.h"
#include "task/TaskStatus.h"
#include "Logging.h"
#include "worker/WorkerDispatcher.h"
#include "persistence/StateStore.h"
#include <type_traits>
#include <random>
#include <sstream>
#include <limits>

namespace GCRP {
namespace {
const std::string* batchId(const Task& task) {
    const auto& payload = task.getPayload();
    if (!payload.is_object() || !payload.contains("context") ||
        !payload["context"].is_object()) return nullptr;
    const auto& context = payload["context"];
    if (!context.contains("batch_id") || !context["batch_id"].is_string()) return nullptr;
    return &context["batch_id"].get_ref<const std::string&>();
}

void countStatus(BatchSnapshot& snapshot, TaskStatus status) {
    ++snapshot.total;
    switch (status) {
        case TaskStatus::PENDING: ++snapshot.pending; break;
        case TaskStatus::QUEUED: ++snapshot.queued; break;
        case TaskStatus::ASSIGNED: ++snapshot.assigned; break;
        case TaskStatus::RUNNING: ++snapshot.running; break;
        case TaskStatus::SUCCEEDED: ++snapshot.succeeded; break;
        case TaskStatus::FAILED: ++snapshot.failed; break;
        case TaskStatus::TIMEOUT: ++snapshot.timeout; break;
        case TaskStatus::CANCELLED: ++snapshot.cancelled; break;
    }
}
}

Scheduler::Scheduler(WorkerManager& worker_manager, size_t max_running_tasks)
    : worker_manager_(worker_manager)
    , max_running_tasks_(max_running_tasks)
    , next_sequence_(0)
{
    if (!max_running_tasks_) throw std::invalid_argument("max running tasks must be positive");
}

void Scheduler::holdWorker(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconciling_workers_.insert(worker_id);
}

void Scheduler::releaseRecoveryHold(const WorkerId& worker_id) {
    for (const auto& entry : tasks_) {
        const auto& task = entry.second;
        if (task.getWorkerId() == worker_id &&
            (task.getStatus() == TaskStatus::ASSIGNED || task.getStatus() == TaskStatus::RUNNING))
            return;
    }
    quarantined_workers_.erase(worker_id);
}

// Caller holds mutex_. A lost attempt is retried only after the Worker proves
// its execution tree is gone, or if START permission was never committed.
std::string Scheduler::recoverLost(Task& task, int max_retries) {
    if (!store_ || task.getRunId().empty())
        throw std::runtime_error("durable recovery requires persistence");
    const auto worker_id = task.getWorkerId();
    const bool recovered = task.recoveryRequired();
    const bool running = task.getStatus() == TaskStatus::RUNNING;
    const auto batch = batchId(task);
    const bool stopped = batch && stopped_batches_.count(*batch);
    const bool retry = !stopped && (!running || task.getRetryCount() < max_retries);
    const auto status = stopped ? TaskStatus::CANCELLED : retry ? TaskStatus::QUEUED : TaskStatus::FAILED;
    const std::string action = stopped ? "cancelled" : retry ? "requeued" : "failed";
    Task prepared = task;
    prepared.restore(status, retry || stopped ? "" : worker_id,
                     task.getRetryCount() + (retry && running ? 1 : 0), task.getPayload(),
                     {{"reason", "worker_lost"}}, retry ? next_sequence_ : task.getQueueSequence(),
                     task.getRunId(), false, task.getCreatedAt(), task.getStartedAt(),
                     retry ? std::chrono::system_clock::time_point{} : std::chrono::system_clock::now());
    if (retry) task_queue_.reserve(tasks_.size() + 1);
    store_->save({prepared}, std::nullopt, "FAILED", std::nullopt,
                 std::make_pair(worker_id, action));
    task = std::move(prepared);
    ++task_generation_;
    if (!recovered) worker_manager_.releaseSlot(worker_id);
    if (retry) task_queue_.push({task.getId(), task.getPriority(), next_sequence_++});
    return action;
}

nlohmann::json Scheduler::recoverWorker(const WorkerId& worker_id, const std::string& state,
        TaskId task_id, const std::string& run_id, int max_retries) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (max_retries < 0 || max_retries > 100) throw std::runtime_error("invalid recovery retry limit");
    if (state == "idle") {
        for (auto& entry : tasks_) {
            auto& task = entry.second;
            if (task.getWorkerId() != worker_id) continue;
            if (task.getStatus() == TaskStatus::RUNNING)
                return {{"action", "quarantined"}, {"task_id", task.getId()}, {"run_id", task.getRunId()}};
            if (task.getStatus() == TaskStatus::ASSIGNED) {
                if (dispatching_.count(task.getId())) return {{"action", "quarantined"}};
                recoverLost(task, max_retries);
            }
        }
        releaseRecoveryHold(worker_id);
        reconciling_workers_.erase(worker_id);
        return {{"action", "ready"}};
    }
    if (state == "lost" && store_) {
        if (auto prior = store_->recoveryDecision(run_id, task_id, worker_id))
            return {{"action", *prior}};
    }
    auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second.getWorkerId() != worker_id ||
        it->second.getRunId() != run_id ||
        (it->second.getStatus() != TaskStatus::ASSIGNED && it->second.getStatus() != TaskStatus::RUNNING))
        return {{"action", "rejected"}};
    if (state == "active") {
        reconciling_workers_.erase(worker_id);
        return {{"action", "resume"}};
    }
    if (state != "lost" || dispatching_.count(task_id)) return {{"action", "rejected"}};
    reconciling_workers_.insert(worker_id);
    return {{"action", recoverLost(it->second, max_retries)}};
}

void Scheduler::restoreFrom(StateStore& store) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tasks_.empty() || store_) throw std::runtime_error("scheduler already initialized");
    auto loaded = store.loadTasks(0,true);
    auto stopped = store.loadStoppedBatches();
    TaskQueue queue;
    queue.reserve(loaded.size());
    for (const auto& entry : loaded) {
        if (entry.task.getStatus() == TaskStatus::QUEUED)
            queue.push({entry.task.getId(), entry.task.getPriority(), entry.queue_sequence});
        if (entry.task.recoveryRequired()) quarantined_workers_.insert(entry.task.getWorkerId());
        next_sequence_ = std::max(next_sequence_, entry.queue_sequence + 1);
        tasks_.emplace(entry.task.getId(), entry.task);
    }
    for (const auto& id : stopped) stopped_batches_.insert(id);
    task_queue_.swap(queue);
    store_ = &store;
    next_sequence_=std::max(next_sequence_,store.highWaterMarks().second+1);
    ++task_generation_;
}

TaskId Scheduler::nextTaskId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskId maximum = 0;
    if(store_)maximum=store_->highWaterMarks().first;
    for (const auto& entry : tasks_) maximum = std::max(maximum, entry.first);
    if (maximum == std::numeric_limits<int64_t>::max())
        throw std::runtime_error("task id exhausted");
    return maximum + 1;
}

std::optional<WorkerId> Scheduler::archiveOwner(TaskId id, const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    if(run_id.empty())return std::nullopt;
    if(it==tasks_.end())return store_?store_->archiveOwner(id,run_id):std::nullopt;
    const auto& task = it->second;
    if (task.getRunId() == run_id &&
        (task.getStatus() == TaskStatus::SUCCEEDED ||
         task.getStatus() == TaskStatus::FAILED || task.getStatus() == TaskStatus::TIMEOUT))
        return task.getWorkerId();
    if (store_) return store_->archiveOwner(id, run_id);
    return std::nullopt;
}

static std::string newRunId(TaskId id) {
    std::random_device source;
    std::ostringstream result;
    result << std::hex << id << '-' << source() << source() << source() << source();
    return result.str();
}

bool Scheduler::submitTask(Task task) {
    return submitTask(std::move(task), "", "").has_value();
}

std::optional<TaskId> Scheduler::findSubmittedTask(const std::string& submission_key,
                                                   const std::string& request_json) const {
    if (submission_key.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!store_) return std::nullopt;
    auto prior = store_->findSubmission(submission_key);
    if (prior && prior->second == request_json) return prior->first;
    return std::nullopt;
}

std::optional<TaskId> Scheduler::submitTask(Task task, const std::string& submission_key,
                                            const std::string& request_json) {
    if (task.getId() == 0) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot submit task with zero TaskId");
        return std::nullopt;
    }

    if (task.getStatus() != TaskStatus::PENDING) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot submit task not in PENDING state");
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (store_ && !submission_key.empty()) {
        auto prior = store_->findSubmission(submission_key);
        if (prior) return prior->second == request_json ?
            std::optional<TaskId>(prior->first) : std::nullopt;
    }

    const auto batch = batchId(task);
    if (batch && (stopped_batches_.count(*batch)||(store_&&store_->batchDeleted(*batch)))) return std::nullopt;

    if(store_){
        if(!store_->loadTasks(task.getId()).empty())return std::nullopt;
        // Only terminal records may leave the hot set. Durable history remains
        // queryable; reservations, active journals and queued tasks stay resident.
        if(tasks_.size()>2048)for(auto it=tasks_.begin();it!=tasks_.end();){
            const auto s=it->second.getStatus();
            if((s==TaskStatus::SUCCEEDED||s==TaskStatus::FAILED||s==TaskStatus::TIMEOUT||s==TaskStatus::CANCELLED)&&!dispatching_.count(it->first))it=tasks_.erase(it);else ++it;
        }
    }

    if (tasks_.find(task.getId()) != tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Duplicate TaskId: " + std::to_string(task.getId()));
        return std::nullopt;
    }

    if (!task.transitionTo(TaskStatus::QUEUED)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition PENDING -> QUEUED failed");
        return std::nullopt;
    }

    // Reserve before changing task ownership; rollback can then requeue without allocation.
    task_queue_.reserve(tasks_.size() + 1);
    const auto id = task.getId();
    const auto priority = task.getPriority();
    task.setQueueSequence(next_sequence_);
    auto inserted = tasks_.emplace(id, std::move(task));
    try {
        if (store_) store_->save({inserted.first->second}, std::nullopt, std::nullopt,
                                submission_key.empty() ? std::nullopt :
                                std::make_optional(std::make_pair(submission_key, request_json)));
    } catch (...) {
        tasks_.erase(id);
        throw;
    }
    task_queue_.push({id, priority, next_sequence_++});
    ++task_generation_;

    return id;
}

std::vector<TaskAssignment> Scheduler::schedule() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t active = 0;
    for (const auto& row : tasks_)
        if (row.second.getStatus() == TaskStatus::ASSIGNED ||
            row.second.getStatus() == TaskStatus::RUNNING) ++active;
    const size_t remaining = active >= max_running_tasks_ ? 0 : max_running_tasks_ - active;
    // Work on a copy: allocation failure must not consume the real queue.
    auto pending = task_queue_;
    pending.reserve(tasks_.size()); // also reserves allocation-free rollback space
    std::vector<TaskAssignment> assignments;
    std::vector<std::pair<Task*, Task>> changes;
    assignments.reserve(pending.size());
    changes.reserve(pending.size());
    size_t acquired = 0;
    std::string selected_after = last_selected_worker_;
    static_assert(std::is_nothrow_move_assignable<Task>::value, "task commit must not throw");
    try {
        while (!pending.empty() && assignments.size() < remaining) {
            const auto queued = pending.top();
            auto it = tasks_.find(queued.task_id);
            if (it == tasks_.end() || it->second.getStatus() != TaskStatus::QUEUED) {
                pending.pop();
                continue;
            }
            auto workers = worker_manager_.getAvailableWorkers();
            if (workers.empty()) break;
            const Worker* selected = nullptr;
            for (const auto& worker : workers) {
                if (quarantined_workers_.count(worker.getId()) ||
                    reconciling_workers_.count(worker.getId())) continue;
                if (!selected || worker.freeSlots() > selected->freeSlots()) {
                    selected = &worker;
                } else if (worker.freeSlots() == selected->freeSlots()) {
                    const bool candidate_after = worker.getId() > selected_after;
                    const bool selected_is_after = selected->getId() > selected_after;
                    if ((candidate_after && !selected_is_after) ||
                        (candidate_after == selected_is_after && worker.getId() < selected->getId()))
                        selected = &worker;
                }
            }
            if (!selected) break;
            const auto worker_id = selected->getId();
            Task prepared = it->second;
            prepared.restore(TaskStatus::QUEUED, "", prepared.getRetryCount(), prepared.getPayload(),
                             nullptr, prepared.getQueueSequence(), "", false, prepared.getCreatedAt(), {}, {});
            prepared.setWorkerId(worker_id);
            if (store_) prepared.setRunId(newRunId(prepared.getId()));
            prepared.transitionTo(TaskStatus::ASSIGNED);
            changes.emplace_back(&it->second, std::move(prepared));
            assignments.push_back({queued.task_id, worker_id});
            if (!worker_manager_.acquireSlot(worker_id)) {
                assignments.pop_back();
                changes.pop_back();
                break;
            }
            ++acquired;
            selected_after = worker_id;
            pending.pop();
        }
    } catch (...) {
        // Strings, vector growth, worker snapshots and queue copies can all fail.
        for (size_t i = 0; i < acquired; ++i)
            worker_manager_.releaseSlot(assignments[i].worker_id);
        throw;
    }
    try {
        if (store_ && !changes.empty()) {
            std::vector<Task> persisted;
            persisted.reserve(changes.size());
            for (const auto& change : changes) persisted.push_back(change.second);
            store_->save(persisted);
        }
    } catch (...) {
        for (size_t i = 0; i < acquired; ++i)
            worker_manager_.releaseSlot(assignments[i].worker_id);
        throw;
    }
    // All potentially allocating work is complete. No logging after commit.
    for (auto& change : changes) *change.first = std::move(change.second);
    if (!changes.empty()) ++task_generation_;
    task_queue_.swap(pending);
    last_selected_worker_.swap(selected_after);
    return assignments;
}

std::optional<bool> Scheduler::dispatchAssignment(
    const TaskAssignment& assignment, WorkerDispatcher& dispatcher) {
    std::optional<Task> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(assignment.task_id);
        if (it == tasks_.end() || it->second.getStatus() != TaskStatus::ASSIGNED ||
            it->second.getWorkerId() != assignment.worker_id ||
            dispatching_.count(assignment.task_id)) return std::nullopt;
        snapshot = it->second; // allocate before establishing the claim
        dispatching_.insert(assignment.task_id);
    }
    try {
        const bool accepted = dispatcher.dispatch(assignment.worker_id, *snapshot);
        std::lock_guard<std::mutex> lock(mutex_);
        dispatching_.erase(assignment.task_id);
        return accepted;
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatching_.erase(assignment.task_id);
        throw;
    }
}


bool Scheduler::completeTask(TaskId task_id, TaskStatus final_status,
                             nlohmann::json execution_summary) {
    if (!isValidFinalStatus(final_status)) {
        LOG_WARN(LogModule::SCHEDULER, "Invalid completion status: " + toString(final_status));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot complete unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    if (task.getStatus() != TaskStatus::RUNNING) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot complete task not in RUNNING state: " + std::to_string(task_id));
        return false;
    }

    if (task.getWorkerId().empty()) {
        LOG_ERROR(LogModule::SCHEDULER, "Task in RUNNING state has no worker_id: " + std::to_string(task_id));
        return false;
    }

    if (!task.canTransitionTo(final_status)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition RUNNING -> " + toString(final_status) + " failed for task=" + std::to_string(task_id));
        return false;
    }
    Task prepared = task;
    prepared.setExecutionSummary(execution_summary);
    prepared.transitionTo(final_status);
    prepared.setRecoveryRequired(false);
    if (store_) store_->save({prepared});
    const bool recovered = task.recoveryRequired();
    task = std::move(prepared);
    ++task_generation_;
    releaseRecoveryHold(task.getWorkerId());

    if (!recovered && !worker_manager_.releaseSlot(task.getWorkerId())) {
        LOG_ERROR(LogModule::SCHEDULER, "Worker slot release failed after task completion for task=" + std::to_string(task_id));
        return false;
    }

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " completed status=" + toString(final_status));

    return true;
}

bool Scheduler::cancelTask(TaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_.count(task_id)) return false;

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot cancel unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    switch (task.getStatus()) {
        case TaskStatus::QUEUED: {
            if (store_) {
                Task prepared = task;
                prepared.transitionTo(TaskStatus::CANCELLED);
                store_->save({prepared});
            }
            if (!task.transitionTo(TaskStatus::CANCELLED)) {
                LOG_ERROR(LogModule::SCHEDULER, "Task transition QUEUED -> CANCELLED failed for task=" + std::to_string(task_id));
                return false;
            }
            ++task_generation_;
            LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " cancelled");
            return true;
        }

        case TaskStatus::ASSIGNED:
        case TaskStatus::RUNNING:
            // No remote cancellation ACK exists yet. Never release a live slot.
            return false;

        case TaskStatus::PENDING:
            if (store_) {
                Task prepared = task;
                prepared.transitionTo(TaskStatus::CANCELLED);
                store_->save({prepared});
            }
            if (!task.transitionTo(TaskStatus::CANCELLED)) {
                LOG_ERROR(LogModule::SCHEDULER, "Task transition PENDING -> CANCELLED failed for task=" + std::to_string(task_id));
                return false;
            }
            ++task_generation_;
            LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " cancelled");
            return true;

        case TaskStatus::SUCCEEDED:
        case TaskStatus::FAILED:
        case TaskStatus::TIMEOUT:
        case TaskStatus::CANCELLED:
            LOG_WARN(LogModule::SCHEDULER, "Cannot cancel terminal task: " + std::to_string(task_id));
            return false;

        default:
            LOG_ERROR(LogModule::SCHEDULER, "Unknown task status for cancellation: " + std::to_string(task_id));
            return false;
    }
}

std::optional<BatchSnapshot> Scheduler::queryBatch(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(store_&&store_->batchDeleted(batch_id))return std::nullopt;
    BatchSnapshot snapshot;
    snapshot.stop_requested = stopped_batches_.count(batch_id) != 0;
    if(store_){for(const auto& row:store_->loadTasks(0,false,batch_id))countStatus(snapshot,row.task.getStatus());return snapshot.total||store_->hasBatch(batch_id)?std::make_optional(snapshot):std::nullopt;}
    for (const auto& entry : tasks_)
        if (const auto id = batchId(entry.second); id && *id == batch_id)
            countStatus(snapshot, entry.second.getStatus());
    if (!snapshot.total) {
        bool planned=false;for(const auto& plan:daily_plans_)for(const auto& item:plan.second["batches"])if(item["batch_id"]==batch_id)planned=true;
        if(!planned)return std::nullopt;
    }
    return snapshot;
}

size_t Scheduler::deleteRecords(TaskId id,const std::string& batch_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!store_)throw std::logic_error("record deletion requires durable SQLite storage");
    // The scheduler lock serializes deletion with submit, assignment and results.
    const auto count=store_->deleteRecords(id,batch_id);
    for(auto it=tasks_.begin();it!=tasks_.end();){
        const auto batch=batchId(it->second);
        if(id?it->first==id:batch&&*batch==batch_id)it=tasks_.erase(it);else ++it;
    }
    // Durable stop state also prevents late Clock submissions from resurrecting a batch.
    if(!batch_id.empty())stopped_batches_.insert(batch_id);
    if(count)++task_generation_;
    return count;
}

bool Scheduler::isBatchDeleted(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_&&store_->batchDeleted(batch_id);
}

size_t Scheduler::deletedCount(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_?store_->deletedCount(batch_id):0;
}

std::optional<BatchSnapshot> Scheduler::cancelBatch(const std::string& batch_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    if(store_)found=store_->hasBatch(batch_id);
    else for(const auto& plan:daily_plans_)for(const auto& item:plan.second["batches"])if(item["batch_id"]==batch_id)found=true;
    for (const auto& entry : tasks_)
        if (const auto id = batchId(entry.second); id && *id == batch_id) {
            found = true; break;
        }
    if (!found) return std::nullopt;
    std::vector<Task> changed;
    changed.reserve(tasks_.size());
    for (const auto& entry : tasks_) {
        const auto id = batchId(entry.second);
        if (!id || *id != batch_id) continue;
        if (entry.second.getStatus() == TaskStatus::PENDING ||
            entry.second.getStatus() == TaskStatus::QUEUED) {
            Task prepared = entry.second;
            prepared.transitionTo(TaskStatus::CANCELLED);
            changed.push_back(std::move(prepared));
        }
    }
    const bool already_stopped = stopped_batches_.count(batch_id) != 0;
    stopped_batches_.insert(batch_id); // Allocate before changing any task state.
    try { if (store_) store_->save(changed, batch_id); }
    catch (...) {
        if (!already_stopped) stopped_batches_.erase(batch_id);
        throw;
    }
    BatchSnapshot snapshot;
    snapshot.stop_requested = true;
    for (auto& entry : tasks_) {
        auto& task = entry.second;
        const auto id = batchId(task);
        if (!id || *id != batch_id) continue;
        if (task.getStatus() == TaskStatus::PENDING || task.getStatus() == TaskStatus::QUEUED) {
            task.transitionTo(TaskStatus::CANCELLED);
            ++snapshot.cancelled_now;
        }
        countStatus(snapshot, task.getStatus());
    }
    if (snapshot.cancelled_now) ++task_generation_;
    if(store_){const auto cancelled=snapshot.cancelled_now;snapshot=BatchSnapshot{};snapshot.stop_requested=true;snapshot.cancelled_now=cancelled;for(const auto& row:store_->loadTasks(0,false,batch_id))countStatus(snapshot,row.task.getStatus());}
    return snapshot;
}

bool Scheduler::isBatchStopped(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_batches_.count(batch_id) != 0;
}

std::optional<Task> Scheduler::getTask(TaskId id,bool include_deleted) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        if(store_){auto rows=store_->loadTasks(id,false,"",include_deleted);if(!rows.empty())return std::move(rows.front().task);}
        return std::nullopt;
    }

    return it->second;
}

std::vector<Task> Scheduler::getTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if(store_){std::vector<Task> result;for(auto& row:store_->loadTasks())result.push_back(std::move(row.task));return result;}

    std::vector<Task> result;
    result.reserve(tasks_.size());

    for (const auto& [id, task] : tasks_) {
        result.push_back(task);
    }

    return result;
}

std::pair<uint64_t, std::vector<Task>> Scheduler::taskSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(store_){std::vector<Task> result;for(auto& row:store_->loadTasks())result.push_back(std::move(row.task));return {task_generation_,std::move(result)};}
    std::vector<Task> result;
    result.reserve(tasks_.size());
    for (const auto& entry : tasks_) result.push_back(entry.second);
    return {task_generation_, std::move(result)};
}

uint64_t Scheduler::taskGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return task_generation_;
}

nlohmann::json Scheduler::statusCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);if(store_)return store_->statusCounts();
    auto result=nlohmann::json::object();for(const auto& task:tasks_){auto key=toString(task.second.getStatus());result[key]=result.value(key,uint64_t(0))+1;}return result;
}

void Scheduler::publishDailyPlan(const nlohmann::json& manifest) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(store_){store_->publishDailyPlan(manifest);return;}
    const auto day=manifest.at("date").get<std::string>();
    const auto it=daily_plans_.find(day);
    if(it!=daily_plans_.end()&&it->second!=manifest)
        throw std::invalid_argument("daily plan already published; resume saved Clock plan");
    daily_plans_[day]=manifest;
}
nlohmann::json Scheduler::dailyPlan(const std::string& day) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(store_)return store_->dailyPlan(day);
    const auto it=daily_plans_.find(day);return it==daily_plans_.end()?nlohmann::json(nullptr):it->second;
}

uint64_t Scheduler::visitTasks(const std::function<void(const Task&)>& visitor,const std::string& batch_id,const std::vector<std::string>& dates) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(store_)store_->visitTasks([&](Task&& task,uint64_t){visitor(task);},0,false,batch_id,dates);
    else for(const auto& entry:tasks_){const auto id=batchId(entry.second);if((batch_id.empty()||(id&&*id==batch_id))&&
        (dates.empty()||std::find(dates.begin(),dates.end(),taskContext(entry.second,"date"))!=dates.end()))visitor(entry.second);}
    return task_generation_;
}

std::optional<std::pair<uint64_t,std::vector<TaskGroupSnapshot>>> Scheduler::taskGroups(
        const std::string& target,const std::string& revision,const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!store_)return std::nullopt;
    return std::make_pair(task_generation_,store_->taskGroups(target,revision,batch_id));
}
uint64_t Scheduler::excludedRegressionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);if(store_)return store_->excludedRegressionCount();
    uint64_t n=0;for(const auto& row:tasks_){const auto& task=row.second;const auto& p=task.getPayload();
        if(p.is_object()&&p.contains("case")&&p["case"].is_string()&&!p["case"].get_ref<const std::string&>().empty()&&
            (taskContext(task,"source")!="scheduled"||taskContext(task,"template").empty()||!validQueryDate(taskContext(task,"date"))))++n;}
    return n;
}

std::vector<Task> Scheduler::getTasksByStatus(TaskStatus status) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(store_){std::vector<Task> result;for(auto& row:store_->loadTasks())if(row.task.getStatus()==status)result.push_back(std::move(row.task));return result;}

    std::vector<Task> result;

    for (const auto& [id, task] : tasks_) {
        if (task.getStatus() == status) {
            result.push_back(task);
        }
    }

    return result;
}

bool Scheduler::isValidFinalStatus(TaskStatus status) const {
    return status == TaskStatus::SUCCEEDED ||
           status == TaskStatus::FAILED ||
           status == TaskStatus::TIMEOUT;
}

bool Scheduler::markTaskStarted(TaskId task_id, const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot start unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    if (task.getStatus() != TaskStatus::ASSIGNED) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot start task not in ASSIGNED state: " + std::to_string(task_id));
        return false;
    }

    if (task.getWorkerId() != worker_id) {
        LOG_WARN(LogModule::SCHEDULER, "Worker id mismatch for task start: task=" + std::to_string(task_id) + " expected=" + task.getWorkerId() + " got=" + worker_id);
        return false;
    }

    Task prepared = task;
    if (!prepared.transitionTo(TaskStatus::RUNNING)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition ASSIGNED -> RUNNING failed for task=" + std::to_string(task_id));
        return false;
    }
    if (store_) store_->save({prepared});
    task = std::move(prepared);
    ++task_generation_;

    LOG_INFO(LogModule::TASK, "task=" + std::to_string(task_id) + " started");

    return true;
}

bool Scheduler::rollbackAssignment(TaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_.count(task_id)) return false;

    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot rollback unknown task: " + std::to_string(task_id));
        return false;
    }

    Task& task = it->second;

    if (task.getStatus() != TaskStatus::ASSIGNED) {
        LOG_WARN(LogModule::SCHEDULER, "Cannot rollback task not in ASSIGNED state: " + std::to_string(task_id));
        return false;
    }

    if (task.getWorkerId().empty()) {
        LOG_ERROR(LogModule::SCHEDULER, "Task in ASSIGNED state has no worker_id: " + std::to_string(task_id));
        return false;
    }

    if (!store_) {
        if (!worker_manager_.releaseSlot(task.getWorkerId())) return false;
        task.clearWorkerId();
        const auto batch = batchId(task);
        if (batch && stopped_batches_.count(*batch)) {
            if (!task.transitionTo(TaskStatus::CANCELLED)) return false;
            ++task_generation_;
            return true;
        }
        if (!task.transitionTo(TaskStatus::QUEUED)) return false;
        task.setQueueSequence(next_sequence_);
        task_queue_.push({task.getId(), task.getPriority(), next_sequence_++});
        ++task_generation_;
        return true;
    }

    Task prepared = task;
    prepared.clearWorkerId();
    const auto batch = batchId(task);
    const bool stopped = batch && stopped_batches_.count(*batch);
    if (!prepared.transitionTo(stopped ? TaskStatus::CANCELLED : TaskStatus::QUEUED)) {
        LOG_ERROR(LogModule::SCHEDULER, "Task transition ASSIGNED -> QUEUED failed for task=" + std::to_string(task_id));
        return false;
    }
    if (!stopped) {
        task_queue_.reserve(tasks_.size() + 1);
        prepared.setQueueSequence(next_sequence_);
    }
    if (store_) store_->save({prepared}, std::nullopt, "DISPATCH_FAILED");
    if (!worker_manager_.releaseSlot(task.getWorkerId()))
        throw std::runtime_error("slot release failed after persisted dispatch rollback");
    task = std::move(prepared);
    ++task_generation_;
    if (!stopped) task_queue_.push({task.getId(), task.getPriority(), next_sequence_++});

    // No allocating diagnostics after the state change.

    return true;
}

} // namespace GCRP
