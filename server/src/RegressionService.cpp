#include "regression/RegressionService.h"
#include "task/TaskType.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>

namespace GCRP {
namespace {
bool readFilter(const nlohmann::json& request, const char* name,
                std::string& value, size_t max_length) {
    if (!request.contains(name)) return true;
    if (!request[name].is_string()) return false;
    value = request[name].get<std::string>();
    return !value.empty() && value.size() <= max_length;
}
std::string field(const nlohmann::json& object, const char* name) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_string()) return {};
    return object[name].get<std::string>();
}
std::string batchOf(const nlohmann::json& payload) {
    if (!payload.is_object() || !payload.contains("context")) return {};
    return field(payload["context"], "batch_id");
}
std::string contextOf(const Task& task,const char* key) {
    const auto& p=task.getPayload();
    return p.is_object()&&p.contains("context")?field(p["context"],key):std::string{};
}
bool validDate(const std::string& value) {
    if(value.size()!=10||value[4]!='-'||value[7]!='-')return false;
    for(size_t i=0;i<10;++i)if(i!=4&&i!=7&&(value[i]<'0'||value[i]>'9'))return false;
    const int y=std::stoi(value.substr(0,4)),m=std::stoi(value.substr(5,2)),d=std::stoi(value.substr(8,2));
    if(y<1||m<1||m>12)return false;
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return d>=1&&d<=days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));
}
std::string outcomeOf(const Task& task) {
    switch (task.getStatus()) {
        case TaskStatus::TIMEOUT: return "TIMEOUT";
        case TaskStatus::CANCELLED: return "CANCELLED";
        case TaskStatus::PENDING:
        case TaskStatus::QUEUED:
        case TaskStatus::ASSIGNED:
        case TaskStatus::RUNNING: return "ACTIVE";
        default: break;
    }
    const auto case_status = field(task.getExecutionSummary(), "case_status");
    if (task.getStatus() == TaskStatus::SUCCEEDED && case_status == "PASS") return "PASS";
    if (task.getStatus() == TaskStatus::FAILED) {
        if (case_status == "FAIL") return "FAIL";
        if (case_status == "CRASH") return "CRASH";
        return "ERROR";
    }
    return "UNKNOWN";
}
int64_t millis(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}
bool matches(const Task& task, const RegressionQuery& filter) {
    if (task.getType() != TaskType::REGRESSION ||
        !task.getPayload().is_object() ||
        field(task.getPayload(), "case").empty()) return false;
    if (!filter.revision.empty() && task.getRevision() != filter.revision) return false;
    if (!filter.case_path.empty() && field(task.getPayload(), "case") != filter.case_path) return false;
    if (!filter.flow.empty() && field(task.getPayload(), "flow") != filter.flow) return false;
    if (!filter.batch_id.empty() && batchOf(task.getPayload()) != filter.batch_id) return false;
    if(!filter.template_name.empty()&&contextOf(task,"template")!=filter.template_name)return false;
    if(!filter.date.empty()&&(contextOf(task,"source")!="scheduled"||contextOf(task,"date")!=filter.date))return false;
    return true;
}
nlohmann::json rowOf(const Task& task) {
    const auto& payload = task.getPayload();
    const auto& execution = task.getExecutionSummary();
    nlohmann::json row = {{"task_id", task.getId()}, {"revision", task.getRevision()},
        {"target", task.getTarget()}, {"case", field(payload, "case")},
        {"flow", field(payload, "flow")}, {"batch_id", batchOf(payload)},
        {"status", toString(task.getStatus())}, {"outcome", outcomeOf(task)},
        {"worker_id", task.getWorkerId()}, {"run_id", task.getRunId()},
        {"retry_count", task.getRetryCount()},
        {"created_at_ms", millis(task.getCreatedAt())}};
    for(const char* key:{"template","source","date"})row[key]=contextOf(task,key);
    if (task.getFinishedAt().time_since_epoch().count() > 0)
        row["finished_at_ms"] = millis(task.getFinishedAt());
    for (const char* key : {"reason", "case_status", "case_reason"}) {
        const auto value = field(execution, key);
        if (!value.empty()) row[key] = value;
    }
    return row;
}
} // namespace

std::optional<RegressionQuery> RegressionQuery::parse(const nlohmann::json& request) {
    if (!request.is_object() || request.size() > 12) return std::nullopt;
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "revision" && it.key() != "case" && it.key() != "flow" &&
            it.key() != "batch_id" && it.key() != "limit" && it.key() != "before_task_id" &&
            it.key() != "date" && it.key() != "baseline_date" && it.key() != "template" &&
            it.key() != "search" && it.key() != "change" && it.key() != "after_key")
            return std::nullopt;
    RegressionQuery result;
    if (!readFilter(request, "revision", result.revision, 128) ||
        !readFilter(request, "case", result.case_path, 512) ||
        !readFilter(request, "flow", result.flow, 64) ||
        !readFilter(request, "batch_id", result.batch_id, 128) ||
        !readFilter(request,"date",result.date,10) ||
        !readFilter(request,"baseline_date",result.baseline_date,10) ||
        !readFilter(request,"template",result.template_name,64) ||
        !readFilter(request,"search",result.search,512) ||
        !readFilter(request,"change",result.change,32) ||
        !readFilter(request,"after_key",result.after_key,1024) ||
        (result.revision.empty() && result.case_path.empty() && result.batch_id.empty() && result.date.empty()))
        return std::nullopt;
    if((!result.date.empty()&&!validDate(result.date))||
       (!result.baseline_date.empty()&&(!validDate(result.baseline_date)||result.date.empty())))return std::nullopt;
    if(result.baseline_date.empty()&&(!result.after_key.empty()||!result.change.empty()||!result.search.empty()))return std::nullopt;
    if(!result.baseline_date.empty()&&(request.contains("before_task_id")||!result.revision.empty()||
       !result.batch_id.empty()||!result.case_path.empty()||!result.flow.empty()))return std::nullopt;
    if(!result.change.empty()&&result.change!="REGRESSION"&&result.change!="IMPROVEMENT"&&
       result.change!="MISSING"&&result.change!="EXCEPTION"&&result.change!="UNCHANGED")return std::nullopt;
    if (request.contains("limit")) {
        if (!request["limit"].is_number_integer()) return std::nullopt;
        const auto limit = request["limit"].get<int64_t>();
        if (limit < 1 || limit > 50) return std::nullopt;
        result.limit = static_cast<size_t>(limit);
    }
    if (request.contains("before_task_id")) {
        if (!request["before_task_id"].is_number_integer()) return std::nullopt;
        if (!request["before_task_id"].is_number_unsigned() &&
            request["before_task_id"].get<int64_t>() <= 0) return std::nullopt;
        result.before_task_id = request["before_task_id"].get<TaskId>();
        if (!result.before_task_id) return std::nullopt;
    }
    return result;
}

nlohmann::json RegressionService::query(const RegressionQuery& filter) const {
    if(!filter.baseline_date.empty())return compareDaily(filter);
    const auto key = nlohmann::json::array({filter.revision, filter.case_path,
        filter.flow, filter.batch_id, filter.limit, filter.before_task_id,filter.date,filter.template_name}).dump();
    if (cache_enabled_ && max_entries_ && ttl_seconds_ > 0) {
        const auto generation = scheduler_.taskGeneration();
        try {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_generation_ != generation) {
                if (!cache_.empty()) ++invalidations_;
                cache_.clear();
                cached_generation_ = generation;
            }
            const auto found = cache_.find(key);
            if (found != cache_.end() && found->second.expires_at > std::chrono::steady_clock::now()) {
                auto response = found->second.response;
                ++hits_;
                return response;
            }
            ++misses_;
        } catch (const std::exception&) {
            // Cache allocation failures must not fail a valid query.
        }
    }
    std::pair<uint64_t,std::vector<Task>> snapshot;
    snapshot.first=scheduler_.visitTasks([&](const Task& task){if(matches(task,filter))snapshot.second.push_back(task);},filter.batch_id,
        filter.date.empty()?std::vector<std::string>{}:std::vector<std::string>{filter.date});
    auto& tasks = snapshot.second;
    std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
        return a.getId() > b.getId();
    });
    nlohmann::json counts = {{"PASS", 0}, {"FAIL", 0}, {"TIMEOUT", 0},
        {"CRASH", 0}, {"ERROR", 0}, {"CANCELLED", 0},
        {"ACTIVE", 0}, {"UNKNOWN", 0}};
    nlohmann::json rows = nlohmann::json::array();
    uint64_t total = 0;
    bool has_more = false;
    for (const auto& task : tasks) {
        if (!matches(task, filter)) continue;
        ++total;
        const auto outcome = outcomeOf(task);
        counts[outcome] = counts[outcome].get<uint64_t>() + 1;
        if (filter.before_task_id && task.getId() >= filter.before_task_id) continue;
        if (rows.size() < filter.limit) rows.push_back(rowOf(task));
        else has_more = true;
    }
    nlohmann::json response = {{"filters", {{"revision", filter.revision},
        {"case", filter.case_path}, {"flow", filter.flow}, {"batch_id", filter.batch_id},
         {"date",filter.date},{"template",filter.template_name}}},
        {"total", total}, {"counts", counts}, {"items", rows},
        {"has_more", has_more}, {"next_before_task_id", nullptr}};
    if (has_more) response["next_before_task_id"] = rows.back()["task_id"];
    response["snapshot_generation"] = snapshot.first;
    if (cache_enabled_ && max_entries_ && ttl_seconds_ > 0 &&
        scheduler_.taskGeneration() == snapshot.first) {
        try {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_generation_ != snapshot.first) {
                cache_.clear();
                cached_generation_ = snapshot.first;
            }
            if (cache_.size() >= max_entries_ && !cache_.count(key)) cache_.erase(cache_.begin());
            cache_[key] = {response, std::chrono::steady_clock::now() +
                                     std::chrono::seconds(ttl_seconds_)};
        } catch (const std::exception&) {
            // The freshly computed response remains authoritative.
        }
    }
    return response;
}

nlohmann::json RegressionService::compareDaily(const RegressionQuery& filter) const {
    using Pair=std::pair<const Task*,const Task*>;
    std::pair<uint64_t,std::vector<Task>> snapshot;std::map<std::string,Pair> pairs;std::set<std::string> templates;
    uint64_t excluded=scheduler_.excludedRegressionCount(),duplicates=0;
    snapshot.first=scheduler_.visitTasks([&](const Task& task){
        if(task.getType()!=TaskType::REGRESSION||field(task.getPayload(),"case").empty())return;
        const auto day=contextOf(task,"date"),stage=contextOf(task,"template");
        if(contextOf(task,"source")!="scheduled"||stage.empty()||!validDate(day))return;
        if(day==filter.date||day==filter.baseline_date)snapshot.second.push_back(task);
    },"",{filter.date,filter.baseline_date});
    for(const auto& task:snapshot.second){
        if(task.getType()!=TaskType::REGRESSION||field(task.getPayload(),"case").empty())continue;
        const auto day=contextOf(task,"date"),stage=contextOf(task,"template");
        if(contextOf(task,"source")!="scheduled"||stage.empty()||!validDate(day)){++excluded;continue;}
        if(day!=filter.date&&day!=filter.baseline_date)continue;
        templates.insert(stage);if(!filter.template_name.empty()&&stage!=filter.template_name)continue;
        auto& pair=pairs[stage+"|"+field(task.getPayload(),"case")];
        auto& side=day==filter.baseline_date?pair.first:pair.second;
        if(side)++duplicates;if(!side||side->getId()<task.getId())side=&task;
        if(filter.date==filter.baseline_date)pair.second=pair.first;
    }
    nlohmann::json rows=nlohmann::json::array(),counts={{"REGRESSION",0},{"IMPROVEMENT",0},
        {"MISSING",0},{"EXCEPTION",0},{"UNCHANGED",0}},stages=nlohmann::json::object();
    uint64_t total=0;bool more=false;std::string last;
    for(const auto& entry:pairs){const auto* a=entry.second.first;const auto* b=entry.second.second;
        const auto ao=a?outcomeOf(*a):"MISSING",bo=b?outcomeOf(*b):"MISSING";
        const std::string change=!a||!b?"MISSING":ao=="PASS"&&bo=="FAIL"?"REGRESSION":
            ao=="FAIL"&&bo=="PASS"?"IMPROVEMENT":bo!="PASS"&&bo!="FAIL"?"EXCEPTION":"UNCHANGED";
        const auto* task=b?b:a;const auto stage=contextOf(*task,"template");
        if(!stages.contains(stage))stages[stage]={{"total",0},{"baseline",nlohmann::json::object()},
            {"comparison",nlohmann::json::object()},{"changes",{{"REGRESSION",0},{"IMPROVEMENT",0},
                {"MISSING",0},{"EXCEPTION",0},{"UNCHANGED",0}}}};
        auto& stats=stages[stage];stats["total"]=stats["total"].get<uint64_t>()+1;
        for(const auto& item:{std::make_pair("baseline",ao),std::make_pair("comparison",bo)}){
            auto& tally=stats[item.first];tally[item.second]=tally.value(item.second,uint64_t(0))+1;}
        stats["changes"][change]=stats["changes"][change].get<uint64_t>()+1;
        if(!filter.search.empty()&&field(task->getPayload(),"case").find(filter.search)==std::string::npos)continue;
        if(!filter.change.empty()&&change!=filter.change)continue;
        ++total;counts[change]=counts[change].get<uint64_t>()+1;
        if(entry.first<=filter.after_key)continue;
        if(rows.size()>=filter.limit){more=true;continue;}
        rows.push_back({{"key",entry.first},{"template",stage},{"case",field(task->getPayload(),"case")},
            {"baseline",a?rowOf(*a):nlohmann::json(nullptr)},{"comparison",b?rowOf(*b):nlohmann::json(nullptr)},
            {"change",change}});last=entry.first;
    }
    return {{"items",rows},{"counts",counts},{"stages",stages},{"templates",templates},{"total",total},
        {"has_more",more},{"next_after_key",more?nlohmann::json(last):nlohmann::json(nullptr)},
        {"snapshot_generation",snapshot.first},{"excluded_unclassified_or_manual",excluded},
        {"duplicate_records",duplicates},{"selection_policy","latest_task_id_per_template_case_date"},
        {"baseline_date",filter.baseline_date},{"date",filter.date}};
}

nlohmann::json RegressionService::cacheStats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return {{"hits", hits_}, {"misses", misses_},
            {"invalidations", invalidations_}, {"entries", cache_.size()}};
}

} // namespace GCRP
