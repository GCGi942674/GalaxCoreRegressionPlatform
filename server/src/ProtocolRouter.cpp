#include "protocol/ProtocolRouter.h"
#include "protocol/ProtocolVersion.h"
#include "task/Task.h"
#include "task/TaskService.h"
#include "task/TaskStatus.h"
#include "task/TaskType.h"
#include "task/RegressionPayload.h"
#include "scheduler/SchedulingDriver.h"
#include "regression/RegressionService.h"
#include "progress/ProgressService.h"
#include "protocol/ApiQueries.h"
#include "protocol/DailyPlan.h"
#include "Logging.h"
#include <chrono>
#include "config/Config.h"
#include <algorithm>
#include <random>
#include <sstream>
#include <limits>
#include <map>

namespace {
const std::string& serverInstanceId() {
    static const std::string id = [] {
        std::random_device random;
        std::ostringstream value;
        value << std::hex;
        for (int i = 0; i < 4; ++i) value << random() << '-';
        return value.str();
    }();
    return id;
}
nlohmann::json batchResult(const std::string& id, const GCRP::BatchSnapshot& s) {
    return {{"batch_id", id}, {"stop_requested", s.stop_requested},
            {"cancelled_now", s.cancelled_now}, {"total", s.total},
            {"pending", s.pending}, {"queued", s.queued},
            {"assigned", s.assigned}, {"running", s.running},
            {"in_flight", s.assigned + s.running},
            {"succeeded", s.succeeded}, {"failed", s.failed},
            {"timeout", s.timeout}, {"cancelled", s.cancelled},
            {"finished", s.succeeded + s.failed + s.timeout},
            {"complete", s.pending + s.queued + s.assigned + s.running == 0}};
}
bool batchRequest(const nlohmann::json& data) {
    return data.is_object() && data.size() == 1 && data.contains("batch_id") &&
           data["batch_id"].is_string() &&
           !data["batch_id"].get<std::string>().empty() &&
           data["batch_id"].get<std::string>().size() <= 128;
}
nlohmann::json executionSummary(const std::string& message, GCRP::TaskStatus status) noexcept {
    // Legacy Workers send the full result, including paths/argv. Bound by the
    // protocol frame, then bound each extracted field; don't discard PASS just
    // because unrelated diagnostic metadata exceeds the summary's size.
    if (message.empty() || message.size() > 1024 * 1024) return nullptr;
    try {
        const auto source = nlohmann::json::parse(message, nullptr, false);
        if (!source.is_object()) return nullptr;
        const auto status_it = source.find("status");
        if (status_it != source.end() &&
            (!status_it->is_string() || status_it->get<std::string>() != GCRP::toString(status)))
            return nullptr;
        nlohmann::json summary = nlohmann::json::object();
        for (const char* key : {"reason", "case_status", "case_reason"}) {
            const auto item = source.find(key);
            if (item != source.end() && item->is_string()) {
                const auto value = item->get<std::string>();
                if (value.size() <= 256) summary[key] = value;
            }
        }
        for (const char* key : {"case_ret_code", "raw_exit_code", "exit_code", "pid"}) {
            const auto item = source.find(key);
            if (item == source.end()) continue;
            if (item->is_number_unsigned()) {
                const auto value = item->get<uint64_t>();
                if (value <= 1000000000) summary[key] = value;
            } else if (item->is_number_integer()) {
                const auto value = item->get<int64_t>();
                if (value >= -1000000000 && value <= 1000000000) summary[key] = value;
            }
        }
        return summary.empty() ? nlohmann::json(nullptr) : summary;
    } catch (...) {
        return nullptr;
    }
}
GCRP::DailyCacheOptions dailyCacheOptions() {
    auto& c=GCRP::Config::instance();GCRP::DailyCacheOptions result;
    result.enabled=c.getBool("cache.daily.enabled",true);
    result.max_entries=static_cast<size_t>(std::max(1,std::min(128,c.getInt("cache.daily.max_entries",8))));
    result.max_bytes=static_cast<size_t>(std::max(4096,std::min(1024*1024*1024,c.getInt("cache.daily.max_bytes",64*1024*1024))));
    result.max_rows=static_cast<size_t>(std::max(100,std::min(200000,c.getInt("cache.daily.max_snapshot_rows",50000))));
    result.ttl_seconds=std::max(1,std::min(60,c.getInt("cache.daily.ttl_seconds",5)));
    result.historical_ttl_seconds=std::max(1,std::min(3600,c.getInt("cache.daily.historical_ttl_seconds",300)));
    return result;
}
}

namespace GCRP::Protocol {

ProtocolRouter::ProtocolRouter(
    TaskService& task_service,
    Scheduler& scheduler,
    WorkerManager& worker_manager,
    SchedulingDriver& scheduling_driver
)
    : task_service_(task_service)
    , scheduler_(scheduler)
    , worker_manager_(worker_manager)
    , scheduling_driver_(scheduling_driver)
    , regression_service_(scheduler,
          Config::instance().getBool("cache.regression.enabled", false),
          static_cast<size_t>(std::max(1, Config::instance().getInt("cache.regression.max_entries", 1000))),
          std::max(1, Config::instance().getInt("cache.regression.ttl_seconds", 5)),dailyCacheOptions())
    , log_archive_(Config::instance().getString("server.log_archive_root", "./data/run-logs"))
{
}

ResponseCode ProtocolRouter::toErrorCode(TaskStatus status) const {
    switch (status) {
        case TaskStatus::SUCCEEDED:
        case TaskStatus::FAILED:
        case TaskStatus::TIMEOUT:
            return ResponseCode::OK;
        default:
            return ResponseCode::INVALID_STATE;
    }
}

ProtocolMessage ProtocolRouter::handle(const ProtocolMessage& request) {
    try {
    if (request.type == GCRP::Protocol::MessageType::RESPONSE) {
        LOG_WARN(LogModule::NETWORK, "Received RESPONSE as incoming request");
        return buildResponse(request, ResponseCode::INVALID_REQUEST, "unexpected response message");
    }

    GCRP::Protocol::ProtocolError error;
    ProtocolMessage response;

    switch (request.type) {
        case MessageType::PUBLISH_DAILY_PLAN: {
            try {
                Api::validateDailyPlan(request.data);
                scheduler_.publishDailyPlan(request.data);
                return buildResponse(request,ResponseCode::OK,"ok",{{"published",true}});
            }catch(const std::invalid_argument& e){
                return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());
            }
        }
        case MessageType::QUERY_DAILY_PLAN: {
            try {
                Api::validate(request.data,{"date","offset","limit"});
                const auto day=Api::text(request.data,"date",10);
                if(!validQueryDate(day))throw std::invalid_argument("invalid date");
                const auto count=Api::limit(request.data);
                uint64_t offset=0;
                if(request.data.contains("offset")){
                    if(!request.data["offset"].is_number_unsigned())throw std::invalid_argument("invalid offset");
                    offset=request.data["offset"].get<uint64_t>();
                }
                auto plan=scheduler_.dailyPlan(day);
                if(plan.is_null())return buildResponse(request,ResponseCode::OK,"ok",
                    {{"date",day},{"plan_available",false},{"items",nlohmann::json::array()},
                     {"note","No published plan: Clock may not have run, or this is legacy history"}});
                auto items=nlohmann::json::array();
                const auto& batches=plan["batches"];
                std::map<std::string,nlohmann::json> counters;
                std::vector<std::tuple<uint64_t,std::string,std::string>> finished_runs;
                for(size_t i=std::min<uint64_t>(offset,batches.size());i<batches.size()&&items.size()<count;++i){
                    items.push_back(batches[i]);counters[batches[i]["batch_id"].get<std::string>()]=
                        {{"submitted",0},{"active",0},{"finished",0},{"cancelled",0},{"logs_verified",0},{"logs_expired",0}};
                }
                const auto generation=scheduler_.visitTasks([&](const Task& task){
                    if(taskContext(task,"source")!="scheduled"||taskContext(task,"date")!=day)return;
                    auto it=counters.find(taskContext(task,"batch_id"));if(it==counters.end())return;
                    auto& c=it->second;auto add=[&](const char* key){c[key]=c[key].get<uint64_t>()+1;};add("submitted");
                    const auto status=task.getStatus();
                    if(status==TaskStatus::CANCELLED)add("cancelled");
                    else if(status==TaskStatus::SUCCEEDED||status==TaskStatus::FAILED||status==TaskStatus::TIMEOUT){
                        add("finished");
                        if(!task.getRunId().empty())finished_runs.emplace_back(task.getId(),task.getRunId(),it->first);
                    }else add("active");
                }, "", {day});
                // Never do filesystem I/O while the scheduler snapshot lock is held.
                // Execution counters share a generation; archive receipts are live observations.
                for(const auto& run:finished_runs){
                    const auto state=log_archive_.list(std::get<0>(run),std::get<1>(run)).value("archive_state",std::string());
                    auto& c=counters.at(std::get<2>(run));
                    if(state=="SERVER_VERIFIED"||state=="LOCAL_CLEANED")c["logs_verified"]=c["logs_verified"].get<uint64_t>()+1;
                    if(state=="EXPIRED")c["logs_expired"]=c["logs_expired"].get<uint64_t>()+1;
                }
                bool all_complete=true;
                for(auto& item:items){
                    const auto id=item["batch_id"].get<std::string>();const auto& c=counters.at(id);
                    for(auto it=c.begin();it!=c.end();++it)item[it.key()]=it.value();
                    item["deleted_records"]=scheduler_.deletedCount(id);
                    const auto planned=item["planned"].get<uint64_t>(), submitted=item["submitted"].get<uint64_t>()+item["deleted_records"].get<uint64_t>();
                    item["not_submitted"]=planned>submitted?planned-submitted:0;
                    const bool stopped=scheduler_.isBatchStopped(id),idle=item["active"]==0;
                    item["stop_requested"]=stopped;
                    item["complete"]=submitted==planned&&idle;
                    item["record_deleted"]=scheduler_.isBatchDeleted(id);
                    item["state"]=item["record_deleted"].get<bool>()?"DELETED":submitted>planned?"COUNT_MISMATCH":stopped?(idle?"STOPPED":"STOPPING"):
                        item["deleted_records"].get<uint64_t>()>0?"PARTIALLY_DELETED":
                        submitted==0?"NOT_SUBMITTED":submitted<planned?"PARTIALLY_SUBMITTED":!idle?"RUNNING":"COMPLETE";
                    all_complete=all_complete&&item["complete"].get<bool>();
                }
                const bool more=offset<batches.size()&&batches.size()-offset>items.size();
                return buildResponse(request,ResponseCode::OK,"ok",{{"date",day},{"plan_available",true},
                    {"suite_id",plan["suite_id"]},{"plan_sha256",plan["plan_sha256"]},{"items",items},
                    {"total",batches.size()},{"has_more",more},{"next_offset",more?nlohmann::json(offset+items.size()):nlohmann::json(nullptr)},
                    {"page_complete",all_complete},{"snapshot_generation",generation}});
            }catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
        }
        case MessageType::RECOVER_OFFLINE_WORKERS: {
            try {
                Api::validate(request.data,{"machine_id","worker_id","confirm_stopped","tasks"});
                const auto machine=Api::text(request.data,"machine_id",256),worker=Api::text(request.data,"worker_id",256);
                if(machine.empty()==worker.empty())throw std::invalid_argument("choose machine_id or worker_id");
                if(!request.data.contains("confirm_stopped") || !request.data["confirm_stopped"].is_boolean() ||
                   !request.data["confirm_stopped"].get<bool>() || !request.data.contains("tasks"))
                    throw std::invalid_argument("operator must confirm original execution has stopped");
                auto result=scheduler_.recoverOfflineWorkers(!machine.empty(),machine.empty()?worker:machine,
                    request.data["tasks"],Config::instance().getInt("server.recovery_max_retries",1));
                LOG_WARN(LogModule::WORKER,"Operator-confirmed offline recovery (trusted LAN, no authentication): "+
                    request.data.dump()+" result="+result.dump());
                try{scheduling_driver_.requestSchedule();}catch(const std::exception& e){LOG_WARN(LogModule::SCHEDULER,e.what());}
                return buildResponse(request,ResponseCode::OK,"ok",result);
            }catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
             catch(const std::logic_error& e){return buildResponse(request,ResponseCode::INVALID_STATE,e.what());}
        }
        case MessageType::CONTROL_WORKERS: {
            try {
                Api::validate(request.data,{"machine_id","worker_id","action","operation_id","scope"});
                const auto machine=Api::text(request.data,"machine_id",256),worker=Api::text(request.data,"worker_id",256);
                if(machine.empty()==worker.empty())throw std::invalid_argument("choose machine_id or worker_id");
                auto result=worker_manager_.control(!machine.empty(),machine.empty()?worker:machine,
                    Api::text(request.data,"action",16),Api::text(request.data,"operation_id",128),Api::text(request.data,"scope",16));
                if(!result)return buildResponse(request,ResponseCode::NOT_FOUND,"worker not found");
                LOG_INFO(LogModule::WORKER,"Worker control accepted (trusted LAN, no authentication): "+request.data.dump());
                try{scheduling_driver_.requestSchedule();}catch(const std::exception& e){LOG_WARN(LogModule::SCHEDULER,e.what());}
                return buildResponse(request,ResponseCode::OK,"ok",*result);
            }catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
             catch(const std::logic_error& e){return buildResponse(request,ResponseCode::INVALID_STATE,e.what());}
        }
        case MessageType::DELETE_TASK:
        case MessageType::DELETE_BATCH: {
            try {
                const bool batch=request.type==MessageType::DELETE_BATCH;
                Api::validate(request.data,batch?std::set<std::string>{"batch_id","confirm"}:std::set<std::string>{"task_id","confirm"});
                if(!request.data.contains("confirm")||!request.data["confirm"].is_boolean()||!request.data["confirm"].get<bool>())
                    throw std::invalid_argument("record deletion requires confirm=true");
                const auto id=batch?0:Api::positive(request.data,"task_id",0);
                const auto name=batch?Api::text(request.data,"batch_id",128):std::string();
                if(batch?name.empty():id==0)throw std::invalid_argument("missing deletion target");
                const auto count=scheduler_.deleteRecords(id,name);
                LOG_INFO(LogModule::TASK,"Record deletion (trusted LAN, no authentication): "+request.data.dump()+" deleted="+std::to_string(count));
                return buildResponse(request,ResponseCode::OK,"ok",{{"deleted",count},{"logical_deletion",true},{"logs_preserved",true}});
            }catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
             catch(const std::logic_error& e){return buildResponse(request,ResponseCode::INVALID_STATE,e.what());}
        }
        case MessageType::LOG_APPEND:
        case MessageType::LOG_STATE:
        case MessageType::QUERY_LOGS:
        case MessageType::READ_LOG: {
            const auto& d = request.data;
            const bool upload = request.type == MessageType::LOG_APPEND;
            const bool read = request.type == MessageType::READ_LOG;
            const bool state = request.type == MessageType::LOG_STATE;
            const size_t expected = state?4:upload ? (d.contains("sha256") ? 8 : 7) : read ? 5 : 2;
            if (!d.is_object() || d.size() != expected ||
                !d.contains("task_id") || !d["task_id"].is_number_unsigned() ||
                d["task_id"].get<uint64_t>() == 0 ||
                !d.contains("run_id") || !d["run_id"].is_string() ||
                !RunLogArchive::validRunId(d["run_id"].get<std::string>()))
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid log identity");
            const auto task_id = d["task_id"].get<uint64_t>();
            const auto run_id = d["run_id"].get<std::string>();
            const auto task = scheduler_.getTask(task_id,true);
            if (!task) return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            const auto owner = scheduler_.archiveOwner(task_id, run_id);
            if (!owner) return buildResponse(request, ResponseCode::INVALID_STATE, "run is not finished");
            try {
                if(state){
                    if(!d.contains("worker_id")||d["worker_id"]!=*owner||!d.contains("state"))
                        return buildResponse(request,ResponseCode::INVALID_REQUEST,"invalid log state owner");
                    return buildResponse(request,ResponseCode::OK,"ok",log_archive_.reportState(task_id,run_id,d["state"]));
                }
                if (!upload && !read)
                    return buildResponse(request, ResponseCode::OK, "ok",
                                         log_archive_.list(task_id, run_id));
                if (!d.contains("name") || !d["name"].is_string() ||
                    !RunLogArchive::validName(d["name"].get<std::string>()) ||
                    !d.contains("offset") || !d["offset"].is_number_unsigned())
                    return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid log file or offset");
                const auto name = d["name"].get<std::string>();
                const auto offset = d["offset"].get<uint64_t>();
                if (read) {
                    if (!d.contains("limit") || !d["limit"].is_number_unsigned() ||
                        d["limit"].get<uint64_t>() < 1 || d["limit"].get<uint64_t>() > 131072)
                        return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid log read limit");
                    return buildResponse(request, ResponseCode::OK, "ok", log_archive_.read(
                        task_id, run_id, name, offset, d["limit"].get<size_t>()));
                }
                if (!d.contains("worker_id") || !d["worker_id"].is_string() ||
                    *owner != d["worker_id"].get<std::string>() ||
                    !d.contains("data_hex") || !d["data_hex"].is_string() ||
                    !d.contains("complete") || !d["complete"].is_boolean() ||
                    (d.contains("sha256") && (!d["sha256"].is_string() ||
                        d["sha256"].get<std::string>().size()!=64)))
                    return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid log upload");
                return buildResponse(request, ResponseCode::OK, "ok", log_archive_.append(
                    task_id, run_id, name, offset, d["data_hex"].get<std::string>(),
                    d["complete"].get<bool>(),d.value("sha256",std::string{})));
            } catch (const std::invalid_argument& error) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, error.what());
            }
        }
        case MessageType::QUERY_CATALOG: {
            try {
                return buildResponse(request, ResponseCode::OK, "ok", gui_catalog_.query(request.data));
            } catch (const std::invalid_argument& e) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, e.what());
            } catch (const std::logic_error& e) {
                return buildResponse(request, ResponseCode::INVALID_STATE, e.what());
            }
        }
        case MessageType::QUERY_TASKS:
        case MessageType::QUERY_WORKERS:
        case MessageType::QUERY_CAPABILITIES: {
            try {
                const auto data = request.type == MessageType::QUERY_TASKS
                    ? task_lists_.query(request.data, scheduler_)
                    : request.type == MessageType::QUERY_WORKERS
                    ? Api::workers(request.data, worker_manager_, &scheduler_)
                    : Api::capabilities(request.data);
                return buildResponse(request, ResponseCode::OK, "ok", data);
            } catch (const std::invalid_argument& e) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, e.what());
            }
        }
        case GCRP::Protocol::MessageType::SUBMIT_TASK: {
            GCRP::Protocol::DTO::SubmitTaskRequest submit_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, submit_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid submit task request");
            }

            GCRP::CreateTaskRequest create_req;
            create_req.type = submit_req.task_type;
            create_req.target = submit_req.target;
            create_req.revision = submit_req.revision;
            create_req.priority = submit_req.priority;
            create_req.payload = submit_req.payload;
            create_req.submission_key = submit_req.submission_key;

            auto result = task_service_.createTask(create_req);
            if (!result) {
                if (validRegressionPayload(create_req.payload) &&
                    create_req.payload.contains("context") &&
                    create_req.payload["context"].contains("batch_id") &&
                    scheduler_.isBatchStopped(create_req.payload["context"]["batch_id"].get<std::string>()))
                    return buildResponse(request, ResponseCode::INVALID_STATE, "batch stopped");
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task parameters");
            }

            nlohmann::json data;
            data["task_id"] = *result;
            return buildResponse(request, ResponseCode::OK, "ok", data);
        }

        case GCRP::Protocol::MessageType::QUERY_TASK: {
            GCRP::Protocol::DTO::QueryTaskRequest query_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, query_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid query task request");
            }

            auto task = task_service_.getTask(query_req.task_id);
            if (!task) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }

            return buildResponse(request, ResponseCode::OK, "ok", taskToJson(*task));
        }

        case GCRP::Protocol::MessageType::QUERY_REGRESSION: {
            auto filter = RegressionQuery::parse(request.data);
            if (!filter)
                return buildResponse(request, ResponseCode::INVALID_REQUEST,
                                     "invalid regression query");
            try{return buildResponse(request, ResponseCode::OK, "ok",regression_service_.query(*filter));}
            catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
        }

        case GCRP::Protocol::MessageType::QUERY_PROGRESS: {
            const auto filter = ProgressQuery::parse(request.data);
            if (!filter)
                return buildResponse(request, ResponseCode::INVALID_REQUEST,
                                     "invalid progress query");
            try{return buildResponse(request, ResponseCode::OK, "ok",ProgressService(scheduler_).queryProgress(*filter));}
            catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
        }

        case GCRP::Protocol::MessageType::QUERY_ROLLBACK: {
            const auto filter = RollbackQuery::parse(request.data);
            if (!filter)
                return buildResponse(request, ResponseCode::INVALID_REQUEST,
                                     "invalid rollback query");
            try{return buildResponse(request, ResponseCode::OK, "ok",ProgressService(scheduler_).queryRollbacks(*filter));}
            catch(const std::invalid_argument& e){return buildResponse(request,ResponseCode::INVALID_REQUEST,e.what());}
        }

        case GCRP::Protocol::MessageType::QUERY_STATUS: {
            if (!request.data.is_object() || !request.data.empty())
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid status request");
            uint64_t queued = 0, assigned = 0, running = 0, pending = 0, total = 0;
            const auto counts=scheduler_.statusCounts();for(auto it=counts.begin();it!=counts.end();++it)total+=it.value().get<uint64_t>();
            pending=counts.value("PENDING",uint64_t(0));queued=counts.value("QUEUED",uint64_t(0));assigned=counts.value("ASSIGNED",uint64_t(0));running=counts.value("RUNNING",uint64_t(0));
            nlohmann::json machines = nlohmann::json::object();
            nlohmann::json workers = nlohmann::json::array();
            for (const auto& worker : worker_manager_.getWorkers()) {
                workers.push_back({{"worker_id", worker.getId()},
                    {"machine_id", worker.getMachineId()}, {"slot_id", worker.getSlotId()},
                    {"status", toString(worker.getStatus())},
                    {"capacity", worker.getTotalSlots()}, {"used", worker.getUsedSlots()}});
                const auto machine = worker.getMachineId();
                if (!machines.contains(machine)) machines[machine] =
                    {{"slots", 0}, {"online", 0}, {"running", 0}};
                machines[machine]["slots"] = machines[machine]["slots"].get<int>() + 1;
                machines[machine]["online"] = machines[machine]["online"].get<int>() +
                    (worker.getStatus() == WorkerStatus::ONLINE ? 1 : 0);
                machines[machine]["running"] = machines[machine]["running"].get<int>() +
                    static_cast<int>(worker.getUsedSlots());
            }
            return buildResponse(request, ResponseCode::OK, "ok",
                                 {{"queued", queued}, {"assigned", assigned},
                                  {"running", running}, {"pending", pending}, {"total", total},
                                  {"workers", workers}, {"machines", machines},
                                  {"regression_cache", regression_service_.cacheStats()}});
        }

        case GCRP::Protocol::MessageType::CANCEL_TASK: {
            GCRP::Protocol::DTO::CancelTaskRequest cancel_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, cancel_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid cancel task request");
            }

            if (!task_service_.getTask(cancel_req.task_id)) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }
            bool success = task_service_.cancelTask(cancel_req.task_id);
            if (!success) {
                return buildResponse(request, ResponseCode::INVALID_STATE,
                    "only PENDING/QUEUED tasks can be cancelled; remote cancellation is not supported");
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case GCRP::Protocol::MessageType::QUERY_BATCH:
        case GCRP::Protocol::MessageType::CANCEL_BATCH: {
            if (!batchRequest(request.data))
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid batch id");
            const auto id = request.data["batch_id"].get<std::string>();
            const auto snapshot = request.type == MessageType::QUERY_BATCH ?
                scheduler_.queryBatch(id) : scheduler_.cancelBatch(id);
            if (!snapshot)
                return buildResponse(request, ResponseCode::NOT_FOUND, "batch not found");
            if (request.type == MessageType::CANCEL_BATCH)
                try { scheduling_driver_.requestSchedule(); }
                catch (const std::exception& error) {
                    LOG_ERROR(LogModule::SCHEDULER, std::string("scheduling delayed: ") + error.what());
                }
            return buildResponse(request, ResponseCode::OK, "ok", batchResult(id, *snapshot));
        }

        case GCRP::Protocol::MessageType::WORKER_REGISTER: {
            GCRP::Protocol::DTO::WorkerRegisterRequest register_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, register_req, error)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid worker register request");
            }

            bool success = worker_manager_.registerWorker(
                register_req.worker_id,
                register_req.hostname,
                register_req.slots, register_req.machine_id, register_req.slot_id
            );
            if (!success) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid worker registration");
            }
            worker_manager_.setControlSupported(register_req.worker_id,register_req.control_v1);
            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case MessageType::WORKER_RECOVER: {
            const auto& d = request.data;
            if (!d.is_object() || !d.contains("worker_id") || !d["worker_id"].is_string() ||
                !d.contains("state") || !d["state"].is_string())
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid recovery request");
            const auto state = d["state"].get<std::string>();
            TaskId task_id = 0;
            std::string run_id;
            if (state != "idle") {
                if ((state != "active" && state != "lost") || !d.contains("task_id") ||
                    !d["task_id"].is_number_unsigned() || d["task_id"].get<TaskId>() == 0 ||
                    !d.contains("run_id") || !d["run_id"].is_string() ||
                    d["run_id"].get<std::string>().empty() || d["run_id"].get<std::string>().size() > 128)
                    return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid recovery identity");
                task_id = d["task_id"].get<TaskId>();
                run_id = d["run_id"].get<std::string>();
            }
            auto result = scheduler_.recoverWorker(d["worker_id"].get<std::string>(), state,
                task_id, run_id, Config::instance().getInt("server.recovery_max_retries", 1));
            if (result["action"] == "ready") scheduling_driver_.requestSchedule();
            return buildResponse(request, ResponseCode::OK, "ok", result);
        }

        case MessageType::WORKER_DRAIN: {
            const auto& data = request.data;
            if (!data.is_object() || data.size() != 1 ||
                !data.contains("worker_id") || !data["worker_id"].is_string() ||
                data["worker_id"].get<std::string>().empty())
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid drain request");
            if (!worker_manager_.markDraining(data["worker_id"].get<std::string>()))
                return buildResponse(request, ResponseCode::NOT_FOUND, "worker not found");
            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case GCRP::Protocol::MessageType::WORKER_HEARTBEAT: {
            GCRP::Protocol::DTO::WorkerHeartbeatRequest heartbeat_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, heartbeat_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid heartbeat request");
            }

            bool success = worker_manager_.heartbeat(heartbeat_req.worker_id);
            if (!success) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "worker not found");
            }
            try {
                auto management=worker_manager_.managementHeartbeat(heartbeat_req.worker_id,
                    request.data.value("maintenance_ack",nlohmann::json(nullptr)));
                scheduling_driver_.requestSchedule();
                return buildResponse(request, ResponseCode::OK, "ok",management);
            }catch(const std::invalid_argument& error){return buildResponse(request,ResponseCode::INVALID_REQUEST,error.what());}
             catch(const std::logic_error& error){return buildResponse(request,ResponseCode::INVALID_STATE,error.what());}
        }

        case GCRP::Protocol::MessageType::TASK_START: {
            GCRP::Protocol::DTO::TaskStartRequest start_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, start_req)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task start request");
            }

            auto task = task_service_.getTask(start_req.task_id);
            if (!task) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }

            if (task->getRunId() != start_req.run_id)
                return buildResponse(request, ResponseCode::INVALID_STATE, "run id mismatch");
            if (task->getWorkerId() != start_req.worker_id)
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "worker_id mismatch");
            if (task->getStatus() != TaskStatus::ASSIGNED && task->getStatus() != TaskStatus::RUNNING) {
                LOG_WARN(LogModule::NETWORK, "TASK_START rejected: task not in ASSIGNED state");
                return buildResponse(request, ResponseCode::INVALID_STATE, "task not ready to start");
            }

            if (task->getWorkerId() != start_req.worker_id) {
                LOG_ERROR(LogModule::NETWORK, "TASK_START rejected: worker_id mismatch");
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "worker_id mismatch");
            }

            bool success = scheduler_.markTaskStarted(start_req.task_id, start_req.worker_id, start_req.run_id);
            if (!success) {
                return buildResponse(request, ResponseCode::INVALID_STATE, "failed to start task");
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        case GCRP::Protocol::MessageType::TASK_RESULT: {
            GCRP::Protocol::DTO::TaskResultRequest result_req;
            if (!GCRP::Protocol::DTO::fromJson(request.data, result_req, error)) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task result request");
            }

            if (result_req.status != TaskStatus::SUCCEEDED &&
                result_req.status != TaskStatus::FAILED &&
                result_req.status != TaskStatus::TIMEOUT) {
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "invalid task result status");
            }

            // A lost RESULT ACK must remain retryable after a GUI record deletion.
            // Hidden terminal rows are only exposed internally for exact run/owner/result checks.
            auto task = scheduler_.getTask(result_req.task_id,true);
            if (!task) {
                return buildResponse(request, ResponseCode::NOT_FOUND, "task not found");
            }

            if (task->getRunId() != result_req.run_id)
                return buildResponse(request, ResponseCode::INVALID_STATE, "run id mismatch");

            if (task->getWorkerId() != result_req.worker_id) {
                LOG_ERROR(LogModule::NETWORK, "TASK_RESULT rejected: worker_id mismatch");
                return buildResponse(request, ResponseCode::INVALID_REQUEST, "worker_id mismatch");
            }

            auto summary = executionSummary(result_req.message, result_req.status);
            if (task->getStatus() == result_req.status &&
                task->getExecutionSummary() == summary) {
                return buildResponse(request, ResponseCode::OK, "ok");
            }
            if (task->getStatus() != TaskStatus::RUNNING) {
                LOG_WARN(LogModule::NETWORK, "TASK_RESULT rejected: task not in RUNNING state");
               return buildResponse(request, ResponseCode::INVALID_STATE, "task not running");
            }
            bool success = scheduler_.completeTask(result_req.task_id, result_req.status, std::move(summary),
                result_req.worker_id,result_req.run_id);
            if (!success) {
                return buildResponse(request, ResponseCode::INTERNAL_ERROR, "failed to complete task");
            }

            try { scheduling_driver_.requestSchedule(); }
            catch (const std::exception& error) {
                LOG_ERROR(LogModule::SCHEDULER, std::string("scheduling delayed: ") + error.what());
            }

            return buildResponse(request, ResponseCode::OK, "ok");
        }

        default:
            return buildResponse(request, ResponseCode::INVALID_REQUEST, "unknown message type");
    }
    } catch (const std::exception& error) {
        LOG_ERROR(LogModule::SERVER, std::string("request persistence failure: ") + error.what());
        return buildResponse(request, ResponseCode::INTERNAL_ERROR, "server state unavailable");
    }
}

ProtocolMessage ProtocolRouter::buildResponse(
    const ProtocolMessage& request,
    ResponseCode code,
    const std::string& message,
    const nlohmann::json& data
) const {
    ProtocolMessage response;
    response.version = GCRP::Protocol::PROTOCOL_VERSION;
    response.type = GCRP::Protocol::MessageType::RESPONSE;
    response.request_id = request.request_id;

    GCRP::Protocol::DTO::Response dto;
    dto.code = static_cast<int>(code);
    dto.message = message;
    dto.data = data;

    response.data = GCRP::Protocol::DTO::toJson(dto);
    response.data["server_instance_id"] = serverInstanceId();
    return response;
}

nlohmann::json ProtocolRouter::taskToJson(const Task& task) const {
    nlohmann::json j;
    j["task_id"] = task.getId();
    j["task_type"] = toString(task.getType());
    j["status"] = toString(task.getStatus());
    j["target"] = task.getTarget();
    j["revision"] = task.getRevision();
    j["worker_id"] = task.getWorkerId();
    if (!task.getRunId().empty()) j["run_id"] = task.getRunId();
    if (task.recoveryRequired()) {
        j["recovery_required"] = true;
        j["recovery_reason"] = "server_restart_pending_reconciliation";
    }
    if (task.getStatus() == TaskStatus::ASSIGNED || task.getStatus() == TaskStatus::RUNNING) {
        const auto worker = worker_manager_.getWorker(task.getWorkerId());
        if (!worker || worker->getStatus() == WorkerStatus::OFFLINE) {
            j["recovery_required"] = true;
            j["recovery_reason"] = "worker_offline";
        }
    }
    j["priority"] = task.getPriority();
    j["payload"] = task.getPayload();
    if (!task.getExecutionSummary().is_null()) j["execution"] = task.getExecutionSummary();
    j["retry_count"] = task.getRetryCount();

    auto created = task.getCreatedAt();
    auto started = task.getStartedAt();
    auto finished = task.getFinishedAt();

    if (created.time_since_epoch().count() > 0) {
        j["created_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            created.time_since_epoch()
        ).count();
    }

    if (started.time_since_epoch().count() > 0) {
        j["started_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            started.time_since_epoch()
        ).count();
    }

    if (finished.time_since_epoch().count() > 0) {
        j["finished_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            finished.time_since_epoch()
        ).count();
    }

    return j;
}

} // namespace GCRP::Protocol
