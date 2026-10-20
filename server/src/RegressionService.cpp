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
       result.change!="MISSING"&&result.change!="EXCEPTION"&&result.change!="UNCHANGED"&&
       result.change!="STABLE_PASS"&&result.change!="STABLE_FAIL"&&result.change!="OTHER")return std::nullopt;
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
    ScopedQueryMetric timing(query_metrics_);
    if(!filter.baseline_date.empty())return compareDaily(filter);
    const auto key = nlohmann::json::array({filter.revision, filter.case_path,
        filter.flow, filter.batch_id, filter.limit, filter.before_task_id,filter.date,filter.template_name}).dump();
    if (cache_enabled_ && max_entries_ && ttl_seconds_ > 0) {
        const auto generation = scheduler_.historyVersion();
        try {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_generation_ != generation) {
                if (!cache_.empty()) ++invalidations_;
                cache_.clear();
                legacy_bytes_=0;
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
    auto history=scheduler_.historySnapshot(filter.batch_id,filter.date.empty()?std::vector<std::string>{}:std::vector<std::string>{filter.date},
        filter.template_name,"",filter.revision.empty()?std::vector<std::string>{}:std::vector<std::string>{filter.revision});
    std::pair<uint64_t,std::vector<Task>> snapshot; snapshot.first=history.generation;
    for(auto& task:history.tasks)if(matches(task,filter))snapshot.second.push_back(std::move(task));
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
        scheduler_.historyVersion() == snapshot.first) {
        try {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_generation_ != snapshot.first) {
                cache_.clear();
                legacy_bytes_=0;
                cached_generation_ = snapshot.first;
            }
            const auto bytes=response.dump().size()*4+512;
            if(bytes<=legacy_budget_){
                if(auto old=cache_.find(key);old!=cache_.end()){legacy_bytes_-=old->second.bytes;cache_.erase(old);}
                while(!cache_.empty()&&(cache_.size()>=max_entries_||legacy_bytes_+bytes>legacy_budget_)){
                    auto old=cache_.begin();legacy_bytes_-=old->second.bytes;cache_.erase(old);
                }
                cache_.emplace(key,CacheEntry{response,std::chrono::steady_clock::now()+std::chrono::seconds(ttl_seconds_),bytes});legacy_bytes_+=bytes;
            }
        } catch (const std::exception&) {
            // The freshly computed response remains authoritative.
        }
    }
    return response;
}

RegressionService::DailyPtr RegressionService::buildDailySnapshot(const RegressionQuery& filter) const {
    ScopedQueryMetric timing(daily_build_metrics_);
    auto history=scheduler_.historySnapshot("",{filter.date,filter.baseline_date},filter.template_name,"",{},true,daily_options_.max_rows);
    auto result=std::make_shared<DailySnapshot>();result->generation=history.generation;
    using Pair=std::pair<const Task*,const Task*>;std::map<std::string,Pair> pairs;
    uint64_t duplicates=0;
    for(const auto& task:history.tasks){
        const auto day=contextOf(task,"date"),stage=contextOf(task,"template"),path=field(task.getPayload(),"case");
        if(task.getType()!=TaskType::REGRESSION||path.empty()||contextOf(task,"source")!="scheduled"||stage.empty()||!validDate(day))continue;
        auto& pair=pairs[stage+"|"+path];auto& side=day==filter.baseline_date?pair.first:pair.second;
        if(side)++duplicates;if(!side||side->getId()<task.getId())side=&task;
        if(filter.date==filter.baseline_date)pair.second=pair.first;
    }
    nlohmann::json rows=nlohmann::json::array(),stages=nlohmann::json::object();
    for(const auto& entry:pairs){
        const auto* a=entry.second.first;const auto* b=entry.second.second;
        const auto ao=a?outcomeOf(*a):"MISSING",bo=b?outcomeOf(*b):"MISSING";
        result->active=result->active||ao=="ACTIVE"||bo=="ACTIVE";
        const std::string change=!a||!b?"MISSING":ao=="PASS"&&bo=="FAIL"?"REGRESSION":
            ao=="FAIL"&&bo=="PASS"?"IMPROVEMENT":bo!="PASS"&&bo!="FAIL"?"EXCEPTION":"UNCHANGED";
        const std::string display=change=="REGRESSION"||change=="IMPROVEMENT"?change:
            ao=="PASS"&&bo=="PASS"?"STABLE_PASS":ao=="FAIL"&&bo=="FAIL"?"STABLE_FAIL":"OTHER";
        const auto* task=b?b:a;const auto stage=contextOf(*task,"template");
        if(!stages.contains(stage))stages[stage]={{"total",0},{"baseline",nlohmann::json::object()},
            {"comparison",nlohmann::json::object()},{"changes",{{"REGRESSION",0},{"IMPROVEMENT",0},{"MISSING",0},{"EXCEPTION",0},{"UNCHANGED",0}}}};
        auto& stats=stages[stage];stats["total"]=stats["total"].get<uint64_t>()+1;
        for(const auto& item:{std::make_pair("baseline",ao),std::make_pair("comparison",bo)}){
            auto& tally=stats[item.first];tally[item.second]=tally.value(item.second,uint64_t(0))+1;
        }
        stats["changes"][change]=stats["changes"][change].get<uint64_t>()+1;
        rows.push_back({{"key",entry.first},{"template",stage},{"case",field(task->getPayload(),"case")},
            {"baseline",a?rowOf(*a):nlohmann::json(nullptr)},{"comparison",b?rowOf(*b):nlohmann::json(nullptr)},
            {"change",change},{"display_change",display}});
    }
    result->data={{"items",std::move(rows)},{"stages",std::move(stages)},{"templates",history.templates},
        {"excluded_unclassified_or_manual",history.excluded},{"duplicate_records",duplicates}};
    // Conservative accounting weight, not a claim to measure allocator RSS.
    result->bytes=result->data.dump().size()*4+4096;
    return result;
}

RegressionService::DailyPtr RegressionService::dailySnapshot(const RegressionQuery& filter) const {
    if(!daily_options_.enabled)return buildDailySnapshot(filter);
    const auto key=nlohmann::json::array({filter.baseline_date,filter.date,filter.template_name}).dump();
    const auto generation=scheduler_.historyVersion({filter.date,filter.baseline_date});
    const auto flight_key=key+":"+std::to_string(generation);
    std::shared_future<DailyPtr> future;std::shared_ptr<std::promise<DailyPtr>> producer;
    {
        std::lock_guard<std::mutex> lock(daily_mutex_);const auto now=std::chrono::steady_clock::now();
        auto found=daily_cache_.find(key);
        if(found!=daily_cache_.end()){
            if(found->second.value->generation==generation&&found->second.expires>now){
                ++daily_hits_;found->second.accessed=now;return found->second.value;
            }
            daily_bytes_-=found->second.value->bytes;daily_cache_.erase(found);++daily_invalidations_;
        }
        auto building=daily_flights_.find(flight_key);
        if(building!=daily_flights_.end()){future=building->second;++daily_waits_;}
        else {producer=std::make_shared<std::promise<DailyPtr>>();future=producer->get_future().share();
            daily_flights_.emplace(flight_key,future);++daily_misses_;++daily_builds_;}
    }
    if(!producer)return future.get(); // No scheduler/database/cache mutex held.
    try{
        auto value=buildDailySnapshot(filter);
        const bool current=scheduler_.historyVersion({filter.date,filter.baseline_date})==value->generation;
        {
            std::lock_guard<std::mutex> lock(daily_mutex_);
            if(current&&daily_options_.max_entries&&value->bytes<=daily_options_.max_bytes){
                if(auto old=daily_cache_.find(key);old!=daily_cache_.end()){
                    daily_bytes_-=old->second.value->bytes;daily_cache_.erase(old);
                }
                while(!daily_cache_.empty()&&(daily_cache_.size()>=daily_options_.max_entries||daily_bytes_+value->bytes>daily_options_.max_bytes)){
                    auto victim=std::min_element(daily_cache_.begin(),daily_cache_.end(),[](const auto& a,const auto& b){return a.second.accessed<b.second.accessed;});
                    daily_bytes_-=victim->second.value->bytes;daily_cache_.erase(victim);++daily_evictions_;
                }
                const auto now=std::chrono::steady_clock::now();
                const auto ttl=value->active?daily_options_.ttl_seconds:daily_options_.historical_ttl_seconds;
                daily_cache_.emplace(key,DailyEntry{value,now+std::chrono::seconds(ttl),now});daily_bytes_+=value->bytes;
            }else if(value->bytes>daily_options_.max_bytes)++daily_oversized_;
            daily_flights_.erase(flight_key);
        }
        producer->set_value(value);return value;
    }catch(...){
        producer->set_exception(std::current_exception());
        {std::lock_guard<std::mutex> lock(daily_mutex_);daily_flights_.erase(flight_key);}throw;
    }
}

nlohmann::json RegressionService::compareDaily(const RegressionQuery& filter) const {
    const auto snapshot=dailySnapshot(filter);const auto& data=snapshot->data;
    nlohmann::json rows=nlohmann::json::array(),counts={{"REGRESSION",0},{"IMPROVEMENT",0},{"MISSING",0},{"EXCEPTION",0},{"UNCHANGED",0}},
        summary={{"ALL",0},{"REGRESSION",0},{"IMPROVEMENT",0},{"STABLE_PASS",0},{"STABLE_FAIL",0},{"OTHER",0}};
    uint64_t total=0;bool more=false;std::string last;
    for(const auto& row:data["items"]){
        const auto& path=row["case"].get_ref<const std::string&>();
        if(!filter.search.empty()&&path.find(filter.search)==std::string::npos)continue;
        const auto& change=row["change"].get_ref<const std::string&>();const auto& display=row["display_change"].get_ref<const std::string&>();
        summary["ALL"]=summary["ALL"].get<uint64_t>()+1;summary[display]=summary[display].get<uint64_t>()+1;
        if(!filter.change.empty()&&change!=filter.change&&display!=filter.change)continue;
        ++total;counts[change]=counts[change].get<uint64_t>()+1;
        const auto& key=row["key"].get_ref<const std::string&>();if(key<=filter.after_key)continue;
        if(rows.size()>=filter.limit){more=true;continue;}rows.push_back(row);last=key;
    }
    return {{"items",rows},{"counts",counts},{"summary_counts",summary},{"stages",data["stages"]},{"templates",data["templates"]},{"total",total},
        {"has_more",more},{"next_after_key",more?nlohmann::json(last):nlohmann::json(nullptr)},
        {"snapshot_generation",snapshot->generation},{"snapshot_scope","requested_dates"},
        {"excluded_unclassified_or_manual",data["excluded_unclassified_or_manual"]},{"duplicate_records",data["duplicate_records"]},
        {"selection_policy","latest_task_id_per_template_case_date"},{"baseline_date",filter.baseline_date},{"date",filter.date}};
}

nlohmann::json RegressionService::cacheStats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::lock_guard<std::mutex> daily_lock(daily_mutex_);
    return {{"hits", hits_}, {"misses", misses_},
            {"invalidations", invalidations_}, {"entries", cache_.size()},
            {"estimated_cached_bytes",legacy_bytes_},{"max_bytes",legacy_budget_},
            {"queries",query_metrics_.json()},{"history_reads",scheduler_.historyStats()},
            {"daily",{{"enabled",daily_options_.enabled},{"hits",daily_hits_},{"misses",daily_misses_},
                {"singleflight_waits",daily_waits_},{"builds",daily_builds_},{"invalidations",daily_invalidations_},
                {"evictions",daily_evictions_},{"oversized_not_cached",daily_oversized_},
                {"entries",daily_cache_.size()},{"inflight",daily_flights_.size()},
                {"estimated_cached_bytes",daily_bytes_},{"max_bytes",daily_options_.max_bytes},
                {"max_entries",daily_options_.max_entries},{"build_timing",daily_build_metrics_.json()}}}};
}

} // namespace GCRP
