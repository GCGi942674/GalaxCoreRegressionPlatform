#include "TestSupport.h"
#include "protocol/TaskListQuery.h"
#include "persistence/StateStore.h"
#include <filesystem>
using namespace GCRP;
int main(){return testMain([]{
    WorkerManager workers;Scheduler scheduler(workers);Api::TaskListQuery query;
    for(uint64_t id=1;id<=5600;++id){Task task(id,TaskType::REGRESSION,"route","19109");task.setPayload({{"case","cases/"+std::to_string(id)+"/run.tcl"},{"flow","route"},{"context",{{"batch_id","daily-entry-"+std::to_string((id-1)/700+1)},{"template","route"},{"source","scheduled"}}}});CHECK(scheduler.submitTask(task));}
    auto page=query.query({{"view","groups"},{"limit",1}},scheduler);
    CHECK(page["total"]==8);CHECK(page["items"][0]["group_summary"]["total"]==700);
    CHECK(scheduler.cancelTask(1));
    auto next=query.query({{"view","groups"},{"limit",1},{"before_task_id",page["next_before_task_id"]},{"snapshot_generation",page["snapshot_generation"]}},scheduler);
    CHECK(next["snapshot_generation"]==page["snapshot_generation"]);CHECK(next["items"][0]["group_summary"]["counts"]["QUEUED"]==700);
    auto detail=query.query({{"batch_id","daily-entry-1"},{"limit",50}},scheduler);size_t total=detail["items"].size();
    while(detail["has_more"].get<bool>()){detail=query.query({{"batch_id","daily-entry-1"},{"limit",50},{"before_task_id",detail["next_before_task_id"]},{"snapshot_generation",detail["snapshot_generation"]}},scheduler);total+=detail["items"].size();}
    CHECK(total==700);
    char pattern[]="/tmp/gcrp-cold-history-XXXXXX";auto path=mkdtemp(pattern);CHECK(path);
    {StateStore store(std::string(path)+"/state.db");std::vector<Task> history;
        for(uint64_t id=1;id<=2200;++id){Task t(id,TaskType::REGRESSION,"target","19109");t.setPayload({{"context",{{"batch_id","history"}}}});CHECK(t.transitionTo(TaskStatus::QUEUED));CHECK(t.transitionTo(TaskStatus::CANCELLED));history.push_back(std::move(t));}
        store.save(history);CHECK(store.loadTasks(0,true).empty());
        WorkerManager restoredWorkers;Scheduler restored(restoredWorkers);restored.restoreFrom(store);
        CHECK(restored.nextTaskId()==2201);CHECK(restored.getTask(1)->getStatus()==TaskStatus::CANCELLED);
        CHECK(restored.queryBatch("history")->cancelled==2200);CHECK(restored.cancelBatch("history")->total==2200);
        CHECK(restored.taskSnapshot().second.size()==2200);
        Api::TaskListQuery persistedQuery;const auto groups=persistedQuery.query({{"view","groups"}},restored);
        CHECK(groups["total"]==1);CHECK(groups["items"][0]["group_summary"]["counts"]["CANCELLED"]==2200);
    }std::filesystem::remove_all(path);
});}
