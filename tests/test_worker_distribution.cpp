#include "scheduler/Scheduler.h"
// Keep test assertions active in Release: several assertions execute the
// registration and scheduling operations needed by later checks.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <set>

using namespace GCRP;
int main() {
    WorkerManager manager;
    Scheduler scheduler(manager);
    for (auto id : {"a-1", "a-2", "b-1", "b-2"}) {
        const std::string worker(id);
        const std::string machine = worker.substr(0, 1);
        const std::string slot = worker.substr(2);
        assert(manager.registerWorker(worker, "host", 1, machine, slot));
    }
    assert(!manager.registerWorker("imposter", "host", 1, "a", "1"));
    assert(!manager.registerWorker("a-1", "host", 1, "b", "1"));
    for (TaskId id = 1; id <= 5; ++id)
        assert(scheduler.submitTask(Task(id, TaskType::REGRESSION, "case", "1")));
    auto first = scheduler.schedule();
    assert(first.size() == 4);
    std::set<std::string> identities;
    for (const auto& item : first) identities.insert(item.worker_id);
    assert(identities == std::set<std::string>({"a-1", "a-2", "b-1", "b-2"}));
    assert(scheduler.getTask(5)->getStatus() == TaskStatus::QUEUED);
    for (const auto& item : first) {
        assert(scheduler.markTaskStarted(item.task_id, item.worker_id));
        assert(manager.getWorker(item.worker_id)->getUsedSlots() == 1);
    }
    assert(manager.markDraining("a-2"));
    assert(manager.heartbeat("a-2"));
    assert(manager.getWorker("a-2")->getStatus() == WorkerStatus::DRAINING);
    for (const auto& item : first)
        assert(scheduler.completeTask(item.task_id, TaskStatus::SUCCEEDED));
    auto next = scheduler.schedule();
    assert(next.size() == 1 && next[0].task_id == 5 && next[0].worker_id != "a-2");
    assert(scheduler.markTaskStarted(5, next[0].worker_id));
    assert(scheduler.completeTask(5, TaskStatus::SUCCEEDED));

    // After all slots become free, equal-capacity slots should rotate.
    std::set<std::string> chosen;
    for (TaskId id = 6; id <= 14; ++id) {
        assert(scheduler.submitTask(Task(id, TaskType::REGRESSION, "case", "1")));
        auto one = scheduler.schedule();
        assert(one.size() == 1);
        chosen.insert(one[0].worker_id);
        assert(one[0].worker_id != "a-2");
        assert(scheduler.markTaskStarted(id, one[0].worker_id));
        assert(scheduler.completeTask(id, TaskStatus::SUCCEEDED));
    }
    assert(chosen == std::set<std::string>({"a-1", "b-1", "b-2"}));
    WorkerManager limitedWorkers;Scheduler limited(limitedWorkers,2);
    for(int i=1;i<=4;++i){assert(limitedWorkers.registerWorker("limited-"+std::to_string(i),"host",1));
        assert(limited.submitTask(Task(i,TaskType::REGRESSION,"limit","1")));}
    auto capped=limited.schedule();assert(capped.size()==2);assert(limited.schedule().empty());
    assert(limited.markTaskStarted(capped[0].task_id,capped[0].worker_id));
    assert(limited.schedule().empty());assert(limited.completeTask(capped[0].task_id,TaskStatus::SUCCEEDED));
    assert(limited.schedule().size()==1);
}
