#pragma once
#include "scheduler/Scheduler.h"
#include "worker/WorkerManager.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <set>
#include <tuple>

namespace GCRP::Api {
using Json = nlohmann::json;
// Throw only validation errors here; Router converts these explicitly to code 1.
inline void validate(const Json& q, const std::set<std::string>& keys) {
    if (!q.is_object()) throw std::invalid_argument("query must be an object");
    for (auto it = q.begin(); it != q.end(); ++it)
        if (!keys.count(it.key())) throw std::invalid_argument("unknown query field");
}
inline std::string text(const Json& q, const char* key, size_t max) {
    if (!q.contains(key)) return {};
    if (!q[key].is_string()) throw std::invalid_argument("expected string filter");
    auto s = q[key].get<std::string>();
    if (s.empty() || s.size() > max || s.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid string filter");
    return s;
}
inline uint64_t positive(const Json& q, const char* key, uint64_t fallback) {
    if (!q.contains(key)) return fallback;
    if (!q[key].is_number_integer() ||
        (!q[key].is_number_unsigned() && q[key].get<int64_t>() <= 0))
        throw std::invalid_argument("expected positive integer");
    auto n = q[key].get<uint64_t>();
    if (!n) throw std::invalid_argument("expected positive integer");
    return n;
}
inline size_t limit(const Json& q) {
    auto n = positive(q, "limit", 20);
    if (n > 50) throw std::invalid_argument("limit exceeds 50");
    return static_cast<size_t>(n);
}
// Bounded list DTO, not the full task payload/flow snapshot. Keeps a 50-row
// response below the 1 MiB frame limit even with highly escaped text.
inline Json taskListSummary(const Task& task) {
    Json payload=Json::object(),execution=Json::object(),context=Json::object();bool truncated=false;
    auto copy=[&truncated](const Json& from,Json& to,const char* key,size_t max){
        if(!from.is_object()||!from.contains(key))return;
        const auto& value=from[key];
        if(value.is_string()) { if(value.get_ref<const std::string&>().size()<=max)to[key]=value;else truncated=true; }
        else if(value.is_number_integer())to[key]=value;
    };
    const auto& full=task.getPayload();copy(full,payload,"case",4096);copy(full,payload,"flow",128);
    if(full.is_object()&&full.contains("context")){
        for(const char* key:{"batch_id","name","template","source","date","suite"})copy(full["context"],context,key,256);
    }
    payload["context"]=std::move(context);
    for(const char* key:{"case_status","case_reason","reason","exit_code","raw_exit_code"})copy(task.getExecutionSummary(),execution,key,256);
    const auto milliseconds=[](const auto& time){return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();};
    Json result={{"created_at_ms",milliseconds(task.getCreatedAt())},{"payload",std::move(payload)},{"execution",std::move(execution)}};
    if(task.getStartedAt().time_since_epoch().count()>0)result["started_at_ms"]=milliseconds(task.getStartedAt());
    if(task.getFinishedAt().time_since_epoch().count()>0)result["finished_at_ms"]=milliseconds(task.getFinishedAt());
    if(result.dump().size()>8192){result["payload"]=Json::object();result["execution"]=Json::object();truncated=true;}
    result["summary_truncated"]=truncated;return result;
}
inline Json tasks(const Json& q, const Scheduler& scheduler) {
    validate(q, {"status", "target", "revision", "batch_id", "limit", "before_task_id"});
    const auto count = limit(q);
    const auto before = positive(q, "before_task_id", 0);
    const auto status = text(q, "status", 16), target = text(q, "target", 128);
    const auto revision = text(q, "revision", 128), batch = text(q, "batch_id", 128);
    if (!status.empty() && !std::set<std::string>{"PENDING", "QUEUED", "ASSIGNED", "RUNNING",
            "SUCCEEDED", "FAILED", "TIMEOUT", "CANCELLED"}.count(status))
        throw std::invalid_argument("unknown task status");
    auto snapshot = scheduler.taskSnapshot();
    auto& rows = snapshot.second;
    std::sort(rows.begin(), rows.end(), [](const Task& a, const Task& b) { return a.getId() > b.getId(); });
    Json items = Json::array();
    size_t total = 0; bool more = false;
    for (const auto& t : rows) {
        std::string task_batch;
        const auto& payload = t.getPayload();
        if (payload.is_object() && payload.contains("context") && payload["context"].is_object()) {
            const auto& ctx = payload["context"];
            if (ctx.contains("batch_id") && ctx["batch_id"].is_string()) task_batch = ctx["batch_id"].get<std::string>();
        }
        if ((!status.empty() && toString(t.getStatus()) != status) ||
            (!target.empty() && t.getTarget() != target) ||
            (!revision.empty() && t.getRevision() != revision) ||
            (!batch.empty() && task_batch != batch)) continue;
        ++total;
        if (before && t.getId() >= before) continue;
        if (items.size() == count) { more = true; continue; }
        items.push_back({{"task_id", t.getId()}, {"task_type", toString(t.getType())},
            {"status", toString(t.getStatus())}, {"target", t.getTarget()},
            {"revision", t.getRevision()}, {"batch_id", task_batch},
            {"worker_id", t.getWorkerId()}, {"run_id", t.getRunId()},
            {"priority", t.getPriority()}, {"retry_count", t.getRetryCount()}});
        const auto summary=taskListSummary(t);
        for(auto it=summary.begin();it!=summary.end();++it)items.back()[it.key()]=it.value();
    }
    return {{"items", items}, {"total", total}, {"has_more", more},
        {"next_before_task_id", more ? items.back()["task_id"] : Json(nullptr)},
        {"snapshot_generation", snapshot.first}};
}
inline Json workers(const Json& q, const WorkerManager& manager, const Scheduler* scheduler=nullptr) {
    validate(q, {"machine_id", "status", "limit", "after"});
    const auto count = limit(q);
    const auto machine = text(q, "machine_id", 256), status = text(q, "status", 16);
    if (!status.empty() && !std::set<std::string>{"ONLINE", "OFFLINE", "DRAINING"}.count(status))
        throw std::invalid_argument("unknown worker status");
    using Key = std::tuple<std::string, std::string, std::string>;
    Key after;
    if (q.contains("after")) {
        const auto& a = q["after"];
        validate(a, {"machine_id", "slot_id", "worker_id"});
        if (a.size() != 3) throw std::invalid_argument("incomplete cursor");
        after = {text(a, "machine_id", 256), text(a, "slot_id", 256), text(a, "worker_id", 256)};
    }
    WorkerInventorySnapshot snapshot;
    if(scheduler)snapshot=scheduler->workerInventory();
    else {auto workers=manager.snapshot();snapshot.worker_generation=workers.first;snapshot.workers=std::move(workers.second);}
    auto& rows = snapshot.workers;
    auto key = [](const Worker& w) { return Key{w.getMachineId(), w.getSlotId(), w.getId()}; };
    std::sort(rows.begin(), rows.end(), [&](const Worker& a, const Worker& b) { return key(a) < key(b); });
    Json items = Json::array(); size_t total = 0; bool more = false;
    for (const auto& w : rows) {
        if ((!machine.empty() && machine != w.getMachineId()) ||
            (!status.empty() && status != toString(w.getStatus()))) continue;
        ++total;
        if (q.contains("after") && key(w) <= after) continue;
        if (items.size() == count) { more = true; continue; }
        items.push_back({{"worker_id", w.getId()}, {"machine_id", w.getMachineId()},
            {"slot_id", w.getSlotId()}, {"hostname", w.getHostname()},
            {"status", toString(w.getStatus())}, {"capacity", w.getTotalSlots()},
            {"used", w.getUsedSlots()}, {"free", w.freeSlots()},
            {"accepting_tasks",w.getStatus()==WorkerStatus::ONLINE&&w.acceptsTasks()}, {"accepting_requested",w.getControl().accepting},
            {"control_supported",w.supportsControl()},
            {"maintenance",{{"operation_id",w.getControl().operation_id},{"scope",w.getControl().scope},
                {"state",w.getControl().state},{"error",w.getControl().error}}},
            {"last_heartbeat_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                w.getLastHeartbeat().time_since_epoch()).count()},
            {"registration_observed",w.getLastHeartbeat().time_since_epoch().count()!=0}});
        if(scheduler){
            const auto active=snapshot.active_tasks.find(w.getId());
            items.back()["current_tasks"]=active==snapshot.active_tasks.end()?Json::array():(*active)["items"];
            const auto total=active==snapshot.active_tasks.end()?uint64_t(0):(*active)["total"].get<uint64_t>();
            items.back()["current_tasks_total"]=total;
            items.back()["current_tasks_truncated"]=total>items.back()["current_tasks"].size();
        }
    }
    Json cursor = nullptr;
    if (more) {
        const auto& w = items.back();
        cursor = {{"machine_id", w["machine_id"]}, {"slot_id", w["slot_id"]}, {"worker_id", w["worker_id"]}};
    }
    return {{"items", items}, {"total", total}, {"has_more", more}, {"next_after", cursor},
        {"snapshot_generation", snapshot.worker_generation+snapshot.task_generation},
        {"task_generation",snapshot.task_generation},
        {"inventory_scope", "current_server_process_and_unreconciled_tasks"}};
}
inline Json capabilities(const Json& q) {
    validate(q, {});
    return {{"api_version", 1}, {"protocol_version", 1},
        {"operations", {"submit_task", "query_task", "query_tasks", "query_workers", "query_status",
            "query_capabilities", "cancel_task", "query_batch", "cancel_batch", "query_regression",
            "query_progress", "query_rollback", "query_catalog", "query_logs", "read_log",
            "publish_daily_plan", "query_daily_plan", "control_workers", "recover_offline_workers", "delete_task", "delete_batch"}},
        {"limits", {{"page_size", 50}, {"progress_page_size", 20}, {"frame_bytes", 1048576},
            {"log_read_bytes",131072},{"log_file_bytes",1073741824}}},
        {"features", {{"remote_cancel", false}, {"cancel_queued", true},
            {"worker_inventory_persistent", false}, {"authentication", false}, {"worker_current_tasks_v1",true},
            {"operator_confirmed_offline_recovery_v1",true},
            {"worker_controls_persistent",true}, {"worker_maintenance_v1",true},
            {"remote_worker_management",true}, {"administration_disabled",false},
            {"archive_sha256_receipts",true}, {"task_group_sql_index",true},
            {"daily_plan_v1",true}, {"archive_lifecycle_v1",true},
            {"record_deletion",true}, {"record_deletion_mode","terminal_only_logical"},
            {"cross_page_snapshot", false}, {"project_scope", false}}}};
}
} // namespace GCRP::Api
