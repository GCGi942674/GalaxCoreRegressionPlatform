#include "persistence/StateStore.h"
#include "scheduler/Scheduler.h"
#include "worker/WorkerManager.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <sqlite3.h>

using namespace GCRP;

int main() {
    char pattern[] = "/tmp/gcrp-p11-XXXXXX";
    char* directory = ::mkdtemp(pattern);
    assert(directory);
    const auto db = std::string(directory) + "/state.db";
    {
        StateStore store(db);
        WorkerManager workers;
        Scheduler scheduler(workers);
        scheduler.restoreFrom(store);
        assert(scheduler.nextTaskId() == 1);
        Task first(1, TaskType::REGRESSION, "case-one", "18925");
        first.setPayload({{"context", {{"batch_id", "batch-1"}}}});
        assert(scheduler.submitTask(first, "batch-1:0", "request-one") == 1);
        assert(scheduler.submitTask(Task(2, TaskType::REGRESSION, "unused", "18925"),
                                    "batch-1:0", "request-one") == 1);
        assert(!scheduler.submitTask(Task(3, TaskType::REGRESSION, "conflict", "18925"),
                                     "batch-1:0", "different-request"));
        Task second(4, TaskType::REGRESSION, "case-two", "18925");
        second.setPayload({{"context", {{"batch_id", "batch-1"}}}});
        assert(scheduler.submitTask(second));
        assert(workers.registerWorker("old-worker", "host", 1));
        auto assignments = scheduler.schedule();
        assert(assignments.size() == 1 && assignments[0].task_id == 1);
        auto run_id = scheduler.getTask(1)->getRunId();
        assert(!run_id.empty());
        assert(scheduler.markTaskStarted(1, "old-worker"));
        auto batch = scheduler.cancelBatch("batch-1");
        assert(batch && batch->cancelled_now == 1 && batch->running == 1);
    }
    {
        StateStore store(db);
        WorkerManager workers;
        Scheduler scheduler(workers);
        scheduler.restoreFrom(store);
        assert(scheduler.nextTaskId() == 5);
        assert(scheduler.isBatchStopped("batch-1"));
        assert(scheduler.getTask(4)->getStatus() == TaskStatus::CANCELLED);
        assert(scheduler.getTask(1)->getStatus() == TaskStatus::RUNNING);
        assert(scheduler.getTask(1)->recoveryRequired());
        assert(!scheduler.submitTask(Task(5, TaskType::REGRESSION, "late", "18925"),
                                     "batch-1:0", "new-request"));
        assert(scheduler.submitTask(Task(5, TaskType::REGRESSION, "new", "18926")));
        assert(workers.registerWorker("old-worker", "host", 1));
        assert(scheduler.schedule().empty());
        assert(workers.registerWorker("new-worker", "host", 1));
        auto assignment = scheduler.schedule();
        assert(assignment.size() == 1 && assignment[0].task_id == 5);
        assert(scheduler.submitTask(Task(6, TaskType::REGRESSION, "write-failure", "18926")));
        sqlite3* injector = nullptr;
        assert(sqlite3_open(db.c_str(), &injector) == SQLITE_OK);
        assert(sqlite3_exec(injector,
            "CREATE TRIGGER fail_task_update BEFORE UPDATE ON tasks BEGIN SELECT RAISE(ABORT,'injected write failure'); END",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        bool failed = false;
        try { scheduler.cancelTask(6); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed && scheduler.getTask(6)->getStatus() == TaskStatus::QUEUED);
        assert(sqlite3_exec(injector, "DROP TRIGGER fail_task_update", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(injector);
        assert(scheduler.cancelTask(6));
        const auto first_run = scheduler.getTask(5)->getRunId();
        assert(scheduler.rollbackAssignment(5));
        assert(scheduler.getTask(5)->getStatus() == TaskStatus::QUEUED);
        auto reassigned = scheduler.schedule();
        assert(reassigned.size() == 1 && reassigned[0].task_id == 5);
        assert(scheduler.getTask(5)->getRunId() != first_run);
        sqlite3* audit = nullptr;
        assert(sqlite3_open(db.c_str(), &audit) == SQLITE_OK);
        sqlite3_stmt* count = nullptr;
        assert(sqlite3_prepare_v2(audit,
            "SELECT COUNT(*) FROM task_runs WHERE task_id=5", -1, &count, nullptr) == SQLITE_OK);
        assert(sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int(count, 0) == 2);
        sqlite3_finalize(count);
        sqlite3_close(audit);
        assert(scheduler.completeTask(1, TaskStatus::SUCCEEDED, {{"reason", "pass"}}));
        assert(!scheduler.getTask(1)->recoveryRequired());
        assert(scheduler.getTask(5)->getStatus() == TaskStatus::ASSIGNED);
    }
    {
        StateStore store(db);
        WorkerManager workers;
        Scheduler scheduler(workers);
        scheduler.restoreFrom(store);
        assert(scheduler.getTask(1)->getStatus() == TaskStatus::SUCCEEDED);
        assert(scheduler.getTask(1)->getExecutionSummary()["reason"] == "pass");
        assert(scheduler.getTask(5)->recoveryRequired());
        assert(scheduler.getTask(6)->getStatus() == TaskStatus::CANCELLED);
    }
    {
        StateStore store(db);WorkerManager workers;Scheduler capped(workers,1);capped.restoreFrom(store);
        assert(workers.registerWorker("spare-slot","spare",1));
        assert(capped.submitTask(Task(7,TaskType::REGRESSION,"waiting","18926")));
        // A recovered ASSIGNED execution also consumes the global reservation.
        assert(capped.schedule().empty());
    }
    {
        sqlite3* legacy=nullptr;assert(sqlite3_open(db.c_str(),&legacy)==SQLITE_OK);
        assert(sqlite3_exec(legacy,"DROP TABLE task_query_index; PRAGMA user_version=3",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(legacy);
        StateStore upgraded(db);
        auto groups=upgraded.taskGroups("","","batch-1");
        assert(groups.size()==1&&groups[0].summary["total"]==2);
        assert(groups[0].summary["counts"]["SUCCEEDED"]==1&&groups[0].summary["counts"]["CANCELLED"]==1);
        auto old=upgraded.loadTasks(1);assert(old.size()==1&&old[0].task.getStatus()==TaskStatus::SUCCEEDED);
        Task dated(8,TaskType::REGRESSION,"date-query","18926");
        dated.setPayload({{"case","case/run.tcl"},{"context",{{"source","scheduled"},{"date","2026-10-18"},{"template","route"}}}});
        assert(dated.transitionTo(TaskStatus::QUEUED));upgraded.save({dated});
        size_t visited=0;upgraded.visitTasks([&](Task&& task,uint64_t){assert(task.getId()==8);++visited;},0,false,"",{"2026-10-18"});
        assert(visited==1);
        sqlite3* audit=nullptr;assert(sqlite3_open(db.c_str(),&audit)==SQLITE_OK);
        sqlite3_stmt* plan=nullptr;
        assert(sqlite3_prepare_v2(audit,"EXPLAIN QUERY PLAN SELECT task_id FROM task_query_index WHERE day='2026-10-18'",-1,&plan,nullptr)==SQLITE_OK);
        assert(sqlite3_step(plan)==SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char*>(sqlite3_column_text(plan,3))).find("query_day")!=std::string::npos);
        sqlite3_finalize(plan);sqlite3_close(audit);
    }
    std::filesystem::remove_all(directory);
}
