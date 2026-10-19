#include "TestSupport.h"
#include "protocol/ApiQueries.h"
#include "protocol/ProtocolCodec.h"
#include "protocol/ConnectionRole.h"
#include "protocol/ProtocolRouter.h"
#include "scheduler/SchedulingCoordinator.h"
#include "scheduler/SchedulingDriver.h"
#include "worker/WorkerDispatcher.h"
using namespace GCRP;
using namespace GCRP::Protocol;
struct Dispatcher : WorkerDispatcher {
    bool dispatch(const WorkerId&, const Task&) override { return false; }
};
int main() { return testMain([] {
    WorkerManager workers; Scheduler scheduler(workers); Dispatcher dispatcher;
    SchedulingCoordinator coordinator(scheduler, dispatcher); SchedulingDriver driver(coordinator);
    TaskService service(scheduler, driver); ProtocolRouter router(service, scheduler, workers, driver);
    auto route = [&](const std::string& type, nlohmann::json data) {
        ProtocolMessage request; ProtocolError error;
        CHECK(ProtocolCodec::decode(envelope(type, data).dump(), request, error));
        CHECK(isAllowed(ConnectionRole::CLIENT, request.type));
        CHECK(!isAllowed(ConnectionRole::WORKER, request.type));
        auto response = router.handle(request);
        CHECK(response.request_id == request.request_id);
        return response.data;
    };
    const auto empty = nlohmann::json::object();
    CHECK(route("query_capabilities", empty)["result"]["features"]["remote_cancel"] == false);
    const auto capabilities=route("query_capabilities",empty)["result"];
    CHECK(capabilities["features"]["administration_disabled"]==true);
    for(const auto& op:capabilities["operations"])CHECK(op!="control_workers");
    for(const auto action:{"pause","resume","update"}){
        CHECK(route("control_workers",{{"machine_id","host"},{"action",action},{"operation_id","blocked"},{"scope","all"}})["code"]==3);
    }
    CHECK(route("query_catalog",empty)["code"]==0);
    CHECK(route("query_catalog",{{"path","/etc"}})["code"]==1);
    CHECK(route("query_tasks", empty)["result"]["items"].empty());
    for (int i = 0; i < 3; ++i)
        CHECK(route("submit_task", {{"task_type", "REGRESSION"}, {"target", "device"}, {"revision", "123"}})["code"] == 0);
    auto page = route("query_tasks", {{"limit", 1}})["result"];
    CHECK(page["items"][0]["created_at_ms"].is_number_integer());
    CHECK(page["items"][0]["payload"].is_object());
    CHECK(page["items"][0]["execution"].is_object());
    CHECK(page["total"] == 3); CHECK(page["has_more"] == true);
    auto next = route("query_tasks", {{"before_task_id", page["next_before_task_id"]}, {"limit", 1}})["result"];
    CHECK(next["items"][0]["task_id"] < page["items"][0]["task_id"]);
    CHECK(next["snapshot_generation"] == page["snapshot_generation"]);
    const auto old_generation = page["snapshot_generation"];
    CHECK(route("cancel_task", {{"task_id", page["items"][0]["task_id"]}})["code"] == 0);
    CHECK(route("query_tasks", empty)["result"]["snapshot_generation"] > old_generation);
    CHECK(route("query_task", {{"task_id", 1u}, {"unexpected", true}})["code"] == 1);
    CHECK(route("query_tasks", empty)["server_instance_id"] == route("query_capabilities", empty)["server_instance_id"]);
    CHECK(route("query_tasks", {{"target", "missing"}})["result"]["total"] == 0);
    for (auto bad : {nlohmann::json{{"limit", 0}}, nlohmann::json{{"limit", 51}},
            nlohmann::json{{"limit", true}}, nlohmann::json{{"before_task_id", -1}},
            nlohmann::json{{"unexpected", 1}}, nlohmann::json{{"status", "invalid"}}}) {
        auto r = route("query_tasks", bad);
        CHECK(r["code"] == 1); CHECK(r["error_reason"] == "INVALID_REQUEST");
    }
    CHECK(workers.registerWorker("host-slot1", "host", 1, "host", "slot1"));
    CHECK(workers.registerWorker("host-slot2", "host", 1, "host", "slot2"));
    CHECK(workers.markOffline("host-slot1")); CHECK(workers.markDraining("host-slot2"));
    page = route("query_workers", {{"limit", 1}})["result"];
    CHECK(page["total"] == 2); CHECK(page["items"][0]["status"] == "OFFLINE");
    CHECK(page["items"][0]["accepting_tasks"]==false);
    CHECK(page["items"][0]["accepting_requested"]==true);
    next = route("query_workers", {{"after", page["next_after"]}})["result"];
    CHECK(next["items"].size() == 1); CHECK(next["items"][0]["status"] == "DRAINING");
    CHECK(next["next_after"].is_null());
    CHECK(workers.heartbeat("host-slot2"));
    CHECK(route("query_workers", empty)["result"]["snapshot_generation"] > next["snapshot_generation"]);
    CHECK(route("query_workers", {{"machine_id", "other"}})["result"]["total"] == 0);
    CHECK(route("query_workers", {{"after", {{"slot_id", "slot1"}}}})["code"] == 1);
    CHECK(route("query_capabilities", {{"extra", true}})["code"] == 1);
    Task fixture(987,TaskType::REGRESSION,"device","19037");
    nlohmann::json payload={{"case","family/device/example/run.tcl"},{"flow","route"},
        {"flow_config",{{"route_design",1}}},{"context",{{"name","manual review"},{"source","manual"},{"template","route"}}}};
    fixture.setPayload(payload);nlohmann::json execution={{"case_status","FAIL"},{"reason","phase error"},{"exit_code",1}};fixture.setExecutionSummary(execution);
    auto summary=Api::taskListSummary(fixture);
    CHECK(summary["payload"]["case"]==payload["case"]);
    CHECK(summary["payload"]["context"]["source"]=="manual");
    CHECK(summary["execution"]["exit_code"]==1);
    CHECK(!summary["payload"].contains("flow_config"));CHECK(fixture.getPayload()==payload);
    payload["case"]=std::string(4096,'\n');fixture.setPayload(payload);summary=Api::taskListSummary(fixture);
    CHECK(summary.dump().size()<=8192);CHECK(summary["summary_truncated"]==true);
    payload["case"]=std::string(5000,'x');fixture.setPayload(payload);summary=Api::taskListSummary(fixture);
    CHECK(!summary["payload"].contains("case"));CHECK(summary["summary_truncated"]==true);
    CHECK(workers.registerWorker("restricted-slot1","restricted",1,"restricted","slot1"));
    workers.setControlSupported("restricted-slot1",true);
    CHECK(workers.control(false,"restricted-slot1","update","saved-before-upgrade","test2").has_value());
    ProtocolMessage heartbeat;heartbeat.type=MessageType::WORKER_HEARTBEAT;heartbeat.request_id=99;
    heartbeat.data={{"worker_id","restricted-slot1"}};
    const auto management=router.handle(heartbeat).data;
    CHECK(management["code"]==0);CHECK(management["result"]["command"].is_null());
    CHECK(management["result"]["administration_disabled"]==true);
    WorkerManager summaryWorkers;Scheduler summaryScheduler(summaryWorkers);
    SchedulingCoordinator summaryCoordinator(summaryScheduler,dispatcher);SchedulingDriver summaryDriver(summaryCoordinator);
    TaskService summaryService(summaryScheduler,summaryDriver);ProtocolRouter summaryRouter(summaryService,summaryScheduler,summaryWorkers,summaryDriver);
    CHECK(summaryWorkers.registerWorker("summary-slot","summary",1));
    CHECK(summaryScheduler.submitTask(Task(1,TaskType::REGRESSION,"long-result","1")));
    CHECK(summaryScheduler.schedule().size()==1);CHECK(summaryScheduler.markTaskStarted(1,"summary-slot"));
    ProtocolMessage resultRequest;resultRequest.type=MessageType::TASK_RESULT;
    resultRequest.data={{"task_id",uint64_t(1)},{"worker_id","summary-slot"},{"status","SUCCEEDED"},
        {"message",nlohmann::json({{"status","SUCCEEDED"},{"case_status","PASS"},{"reason","pass"},{"argv",std::string(9000,'x')}}).dump()}};
    CHECK(summaryRouter.handle(resultRequest).data["code"]==0);
    CHECK(summaryScheduler.getTask(1)->getExecutionSummary()["case_status"]=="PASS");
}); }
