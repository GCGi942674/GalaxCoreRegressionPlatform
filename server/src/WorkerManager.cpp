#include "worker/WorkerManager.h"
#include "task/TaskLimits.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "Logging.h"
#include "persistence/StateStore.h"
#include <set>
#include <stdexcept>
#include "config/Config.h"

namespace GCRP {
namespace {
using Json=nlohmann::json;
Json controlJson(const WorkerControl& c, const std::string& machine) {
    return {{"machine_id",machine},{"accepting",c.accepting},{"operation_id",c.operation_id},
        {"scope",c.scope},{"state",c.state},{"error",c.error}};
}
WorkerControl parseControl(const Json& j) {
    if(!j.is_object()||!j.at("accepting").is_boolean())throw std::runtime_error("invalid persisted Worker control");
    WorkerControl c;c.accepting=j.at("accepting").get<bool>();
    c.operation_id=j.at("operation_id").get<std::string>();c.scope=j.at("scope").get<std::string>();
    c.state=j.at("state").get<std::string>();c.error=j.at("error").get<std::string>();
    if(!std::set<std::string>{"IDLE","PENDING","RUNNING","SUCCEEDED","FAILED"}.count(c.state)||
       c.error.size()>512||c.operation_id.size()>128||
       (c.state!="IDLE"&&(c.operation_id.empty()||(c.scope!="test2"&&c.scope!="all"))))
        throw std::runtime_error("invalid persisted Worker maintenance state");
    return c;
}
}

WorkerManager::WorkerManager(StateStore* store):store_(store),controls_(
    store?store->loadWorkerControls():nlohmann::json{{"machines",Json::object()},{"workers",Json::object()}}) {
    if(!controls_.is_object()||!controls_.at("machines").is_object()||!controls_.at("workers").is_object())
        throw std::runtime_error("invalid persisted Worker management");
    for(auto it=controls_["machines"].begin();it!=controls_["machines"].end();++it)
        if(!it.value().is_boolean())throw std::runtime_error("invalid machine policy");
    for(auto it=controls_["workers"].begin();it!=controls_["workers"].end();++it){
        parseControl(it.value());it.value().at("machine_id").get<std::string>();}
    if(!controls_.contains("requests"))controls_["requests"]=Json::object();
    if(!controls_["requests"].is_object())throw std::runtime_error("invalid control request history");
    default_policy_=MachineRunPolicy::defaults();default_policy_["enabled"]=Config::instance().getBool("policy.enabled",true);
    auto tuning=[&](const char* key,int value,int low,int high){const int n=Config::instance().getInt(key,value);if(n<low||n>high)throw std::invalid_argument(std::string("invalid ")+key);return n;};
    policy_tuning_.stale_seconds=tuning("policy.telemetry_stale_seconds",30,15,300);
    policy_tuning_.stress_seconds=tuning("policy.stress_seconds",60,1,3600);
    policy_tuning_.recover_seconds=tuning("policy.recover_seconds",180,1,3600);
    policy_tuning_.step_seconds=tuning("policy.step_seconds",120,1,3600);
    policy_tuning_.warn_percent=tuning("policy.memory_warn_percent",20,1,95);
    policy_tuning_.critical_percent=tuning("policy.memory_critical_percent",10,1,94);
    policy_tuning_.recover_percent=tuning("policy.memory_recover_percent",30,2,99);
    policy_tuning_.cpu_expand_percent=tuning("policy.cpu_expand_percent",85,1,100);
    policy_tuning_.disk_reserve_bytes=uint64_t(tuning("policy.disk_reserve_mb",5120,64,1048576))*1024*1024;
    if(policy_tuning_.critical_percent>=policy_tuning_.warn_percent||policy_tuning_.warn_percent>=policy_tuning_.recover_percent)throw std::invalid_argument("policy memory thresholds must increase");
    if(!controls_.contains("run_policies"))controls_["run_policies"]=Json::object();
    if(!controls_["run_policies"].is_object())throw std::invalid_argument("invalid saved machine policies");
    for(const auto& s:controls_["run_policies"])MachineRunPolicy::validate(s);
}

namespace {
double policyNow(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int64_t policyWall(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
}
Json WorkerManager::specForLocked(const std::string& machine) const {
    return controls_["run_policies"].contains(machine)?controls_["run_policies"][machine]:default_policy_;
}
bool WorkerManager::refreshPoliciesLocked(){
    bool changed=false;const auto now=policyNow();const auto wall=policyWall();const time_t seconds=wall/1000;std::tm local{};localtime_r(&seconds,&local);
    for(auto& policy:run_policies_){
        auto view=policy.second.evaluate(specForLocked(policy.first),wall,local,now);
        if(!policy_views_.count(policy.first)||view!=policy_views_.at(policy.first)){
            const auto old=policy_views_.find(policy.first);
            if(old==policy_views_.end()||old->second["target_limit"]!=view["target_limit"]||old->second["effective_mode"]!=view["effective_mode"]||old->second["reason"]!=view["reason"])
                LOG_INFO(LogModule::WORKER,"machine policy machine="+policy.first+" mode="+view["effective_mode"].get<std::string>()+" target="+std::to_string(view["target_limit"].get<int>())+" reason="+view["reason"].get<std::string>());
            policy_views_[policy.first]=std::move(view);changed=true;
        }
    }
    if(changed)++generation_;return changed;
}
bool WorkerManager::refreshRunPolicies(){std::lock_guard<std::mutex> lock(mutex_);return refreshPoliciesLocked();}
void WorkerManager::reportResources(const WorkerId& id,const Json& report){
    std::lock_guard<std::mutex> lock(mutex_);const auto w=workers_.find(id);if(w==workers_.end())throw std::invalid_argument("unknown Worker resource report");
    if(!resource_workers_.count(id))throw std::invalid_argument("Worker did not declare resource reporting");
    auto p=run_policies_.find(w->second->getMachineId());
    if(p==run_policies_.end()||!p->second.supported())throw std::invalid_argument("resource_version registration required");
    p->second.report(report,w->second->getMachineId(),policyNow());
    refreshPoliciesLocked();
}
Json WorkerManager::setRunPolicy(const std::string& machine,const std::string& operation,const Json& patch){
    if(machine.empty()||machine.size()>64||operation.empty()||operation.size()>128)throw std::invalid_argument("invalid machine policy target");
    std::lock_guard<std::mutex> lock(mutex_);
    const Json fingerprint={{"machine",machine},{"policy",patch}};
    if(controls_["requests"].contains(operation)){
        const auto& old=controls_["requests"][operation];if(old.at("request")!=fingerprint)throw std::invalid_argument("operation_id reused");return old.at("result");
    }
    const auto p=run_policies_.find(machine);
    const bool unsupported=p==run_policies_.end()||!p->second.supported();
    if(unsupported&&(!controls_["run_policies"].contains(machine)||!patch.is_object()||!patch.contains("enabled")||patch["enabled"]!=false))throw std::logic_error("upgrade Worker first, or explicitly disable saved policy to use legacy manual control");
    const auto spec=MachineRunPolicy::patch(specForLocked(machine),patch,policyWall());
    Json result={{"machine_id",machine},{"operation_id",operation},{"policy_saved",spec}};
    auto next=controls_;next["run_policies"][machine]=spec;next["requests"][operation]={{"request",fingerprint},{"result",result}};
    commitControls(std::move(next));refreshPoliciesLocked();return result;
}
Json WorkerManager::policyViewLocked(const Worker& worker) const {
    const auto machine=worker.getMachineId();const auto p=run_policies_.find(machine);
    if(p==run_policies_.end()){
        const bool required=controls_["run_policies"].contains(machine)&&specForLocked(machine)["enabled"].get<bool>();
        return {{"supported",false},{"enabled",required},{"resume_allowed",!required},{"slot_allowed",!required},{"plan",specForLocked(machine)},{"reason","upgrade_worker"}};
    }
    auto view=policy_views_.at(machine);const auto number=MachineRunPolicy::slotNumber(worker.getSlotId());
    const bool enabled=view["enabled"].get<bool>();const bool mode_allowed=!enabled||(number>0&&number<=p->second.modeLimit());
    std::vector<int> allowed;const uint64_t used=machineUsedLocked(machine);
    for(const auto& row:workers_)if(row.second->getMachineId()==machine){
        const auto n=MachineRunPolicy::slotNumber(row.second->getSlotId());
        if(n>0&&n<=p->second.modeLimit()&&row.second->getStatus()==WorkerStatus::ONLINE&&row.second->acceptsTasks())allowed.push_back(n);
    }
    std::sort(allowed.begin(),allowed.end());const auto found=std::find(allowed.begin(),allowed.end(),number);
    const bool adaptive_allowed=found!=allowed.end()&&std::distance(allowed.begin(),found)<p->second.target();
    const bool compatible=resource_workers_.count(worker.getId())>0;
    view["active_tasks"]=used;view["resume_allowed"]=!enabled||(mode_allowed&&compatible);
    view["slot_allowed"]=!enabled||(mode_allowed&&compatible&&adaptive_allowed&&p->second.fresh(policyNow()));
    if(enabled&&!compatible)view["reason"]="upgrade_worker";return view;
}
uint64_t WorkerManager::machineUsedLocked(const std::string& machine) const {
    uint64_t used=0;
    for(const auto& row:workers_)if(row.second->getMachineId()==machine){
        const auto known=known_task_usage_.find(row.first);
        used+=std::max(row.second->getUsedSlots(),known==known_task_usage_.end()?uint32_t(0):known->second);
    }
    // Resource-capable registration requires canonical machine-slotN identity.
    // Preserve budget for restored runs even before their owner reconnects.
    const auto prefix=machine+"-";
    for(const auto& row:known_task_usage_)if(!workers_.count(row.first)&&row.first.compare(0,prefix.size(),prefix)==0&&
        MachineRunPolicy::slotNumber(row.first.substr(prefix.size()))>0)used+=row.second;
    return used;
}
bool WorkerManager::admitsLocked(const Worker& worker) const {
    const auto p=run_policies_.find(worker.getMachineId());if(p==run_policies_.end())return policyViewLocked(worker).value("slot_allowed",true);
    if(!p->second.active())return true;
    const auto view=policyViewLocked(worker);return view["slot_allowed"].get<bool>()&&view["active_tasks"].get<uint64_t>()<uint64_t(p->second.target());
}

std::shared_ptr<const WorkerControl> WorkerManager::controlFor(const Json& state,
        const WorkerId& id,const std::string& machine) const {
    WorkerControl c;
    if(state.at("workers").contains(id)){
        const auto& row=state.at("workers").at(id);
        // Empty only for an operator-paused historical owner whose machine
        // identity was unavailable after Server restart. The Worker ID stays exact.
        if(row.at("machine_id")!="" && row.at("machine_id")!=machine)
            throw std::invalid_argument("managed Worker machine identity changed");
        c=parseControl(row);
    }else if(state.at("machines").contains(machine))c.accepting=state.at("machines").at(machine).get<bool>();
    return std::make_shared<const WorkerControl>(std::move(c));
}

void WorkerManager::commitControls(Json next) {
    // Prepare every allocating copy before the durable commit; publishing is noexcept.
    std::vector<std::pair<Worker*,std::shared_ptr<const WorkerControl>>> prepared;
    for(auto& row:workers_)prepared.emplace_back(row.second.get(),controlFor(next,row.first,row.second->getMachineId()));
    if(store_)store_->saveWorkerControls(next);
    controls_.swap(next);
    for(auto& row:prepared)row.first->control_=std::move(row.second);
    ++generation_;
}

void WorkerManager::setControlSupported(const WorkerId& id,bool supported) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it=workers_.find(id);if(it!=workers_.end()){it->second->control_supported_=supported;++generation_;}
}

std::optional<Json> WorkerManager::control(bool machine,const std::string& target,
        const std::string& action,const std::string& operation_id,const std::string& scope) {
    if(target.empty()||target.size()>256||operation_id.empty()||operation_id.size()>128||
       (action!="pause"&&action!="resume"&&action!="update")||
       (action=="update"?(scope!="test2"&&scope!="all"):!scope.empty()))
        throw std::invalid_argument("invalid Worker control parameters");
    std::lock_guard<std::mutex> lock(mutex_);
    Json fingerprint={{"machine",machine},{"target",target},{"action",action},{"scope",scope}};
    refreshPoliciesLocked();
    if(controls_["requests"].contains(operation_id)){
        const auto& old=controls_["requests"][operation_id];
        if(old.at("request")!=fingerprint)throw std::invalid_argument("operation_id reused with different parameters");
        return old.at("result");
    }
    std::set<std::string> ids;
    for(const auto& row:workers_)if(machine?row.second->getMachineId()==target:row.first==target)ids.insert(row.first);
    if(ids.empty())return std::nullopt;
    if(machine&&action=="pause")for(auto it=controls_["workers"].begin();it!=controls_["workers"].end();++it)
        if(it.value().at("machine_id")==target)ids.insert(it.key());
    auto next=controls_;Json affected=Json::array(),skipped=Json::array();
    for(const auto& id:ids){
        const auto found=workers_.find(id);
        if(action=="resume"&&(found==workers_.end()||found->second->getStatus()!=WorkerStatus::ONLINE)){
            if(!machine)throw std::logic_error("resume requires an ONLINE slot; offline/draining policy is unchanged");
            skipped.push_back(id);continue;
        }
        if(action=="resume"&&!policyViewLocked(*found->second).value("resume_allowed",true)){
            if(!machine)throw std::logic_error("slot disabled by current machine mode; change mode instead");
            skipped.push_back(id);continue;
        }
        const std::string host=found!=workers_.end()?found->second->getMachineId():next["workers"][id]["machine_id"].get<std::string>();
        WorkerControl c=*controlFor(next,id,host);
        const bool pending=c.state=="PENDING"||c.state=="RUNNING";
        if(action=="update"){
            if(found==workers_.end()||found->second->getStatus()!=WorkerStatus::ONLINE){
                if(machine){skipped.push_back(id);continue;}
                throw std::logic_error("update requires online, non-draining slots");
            }
            if(!found->second->supportsControl()||found->second->getTotalSlots()!=1)
                throw std::logic_error("Worker does not support maintenance v1; upgrade Worker first");
            if(pending)throw std::logic_error("another maintenance operation is pending");
            c.operation_id=operation_id;c.scope=scope;c.state="PENDING";c.error.clear();
        }else if(action=="pause")c.accepting=false;
        else {
            if(pending)throw std::logic_error("cannot resume before maintenance finishes");
            c.accepting=true;c.operation_id.clear();c.scope.clear();c.state="IDLE";c.error.clear();
        }
        next["workers"][id]=controlJson(c,host);affected.push_back(id);
    }
    // Resume is a snapshot operation, not an enable-all policy for offline or future slots.
    if(machine&&action=="pause")next["machines"][target]=false;
    Json result={{"operation_id",operation_id},{"action",action},{"worker_ids",affected},
        {"accepted",affected.size()},{"skipped_worker_ids",skipped},
        {"note",action=="update"?"queued; wait for Worker acknowledgement":action=="resume"?
            "only ONLINE slots resumed; OFFLINE/DRAINING and future slot policies unchanged":"policy saved; active cases are not terminated"}};
    next["requests"][operation_id]={{"request",fingerprint},{"result",result}};
    commitControls(std::move(next));return result;
}

Json WorkerManager::managementHeartbeat(const WorkerId& id,const Json& ack) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found=workers_.find(id);if(found==workers_.end())throw std::invalid_argument("unknown Worker");
    const auto reserved=known_task_usage_.find(id);
    const bool taskOwned=found->second->getUsedSlots()!=0||
        (reserved!=known_task_usage_.end()&&reserved->second!=0);
    if(!ack.is_null()){
        if(!ack.is_object()||ack.size()!=3||!ack.contains("operation_id")||!ack["operation_id"].is_string()||
           !ack.contains("state")||!ack["state"].is_string()||!ack.contains("error")||!ack["error"].is_string()||
           ack["error"].get<std::string>().size()>512||ack["operation_id"].get<std::string>().size()>128)
            throw std::invalid_argument("invalid maintenance acknowledgement");
        const auto phase=ack["state"].get<std::string>();
        if(phase!="RUNNING"&&phase!="SUCCEEDED"&&phase!="FAILED")throw std::invalid_argument("invalid maintenance phase");
        const auto& current=found->second->getControl();
        if(!current.operation_id.empty()&&ack["operation_id"]==current.operation_id&&
           (current.state=="PENDING"||current.state=="RUNNING")){
            if(taskOwned)throw std::logic_error("maintenance cannot run while a task owns the slot");
            auto next=controls_;auto& row=next["workers"][id];
            row["state"]=phase;row["error"]=ack["error"];
            if(row!=controls_["workers"][id])commitControls(std::move(next));
        }
    }
    const auto& c=found->second->getControl();
    Json command=nullptr;
    if(c.state=="PENDING"&&!taskOwned)
        command={{"operation_id",c.operation_id},{"scope",c.scope}};
    return {{"accepting_tasks",c.available()},{"maintenance_state",c.state},{"command",command}};
}

bool WorkerManager::registerWorker(
    const WorkerId& worker_id,
    const std::string& hostname,
    uint32_t slots,
    const std::string& machine_id,
    const std::string& slot_id, int machine_slots
) {
    if (worker_id.empty() || worker_id.size() > kMaxWorkerIdBytes ||
        hostname.size() > kMaxWorkerHostnameBytes) {
        LOG_ERROR(LogModule::WORKER, "Invalid worker registration: empty or oversized identity");
        return false;
    }

    if (hostname.empty()) {
        LOG_ERROR(LogModule::WORKER, "Invalid worker registration: empty hostname");
        return false;
    }

    if (slots == 0 || machine_slots<0 || machine_slots>128 || (machine_slots>0&&(slots!=1||machine_id.empty()||slot_id.empty()||worker_id!=machine_id+"-"+slot_id||MachineRunPolicy::slotNumber(slot_id)==0)) || (!machine_id.empty() && machine_id.size() > 64) ||
        (!slot_id.empty() && slot_id.size() > 64)) {
        LOG_ERROR(LogModule::WORKER, "Invalid worker registration: slots must be > 0");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(worker_id);
    const auto machine = machine_id.empty() ? hostname : machine_id;
    const auto slot = slot_id.empty() ? worker_id : slot_id;
    const auto control=controlFor(controls_,worker_id,machine);
    for (const auto& entry : workers_) {
        if (entry.first != worker_id && entry.second->getMachineId() == machine &&
            entry.second->getSlotId() == slot) return false;
    }
    if(machine_slots>0){
        if(!controls_["run_policies"].contains(machine)){
            auto next=controls_;next["run_policies"][machine]=default_policy_;commitControls(std::move(next));
        }
        run_policies_.try_emplace(machine,policy_tuning_).first->second.declareSlots(machine_slots);refreshPoliciesLocked();
        resource_workers_.insert(worker_id);
    }
    else resource_workers_.erase(worker_id);
    if (it != workers_.end()) {
        if (it->second->hasExplicitIdentity() &&
            (it->second->getMachineId() != machine || it->second->getSlotId() != slot))
            return false;
        // Re-registration: update existing worker
        it->second->hostname_ = hostname;
        it->second->setIdentity(machine, slot, !machine_id.empty() && !slot_id.empty());
        it->second->setTotalSlots(slots);
        it->second->setOnline();
        it->second->control_=control;
        it->second->updateHeartbeat();
        ++generation_;
        LOG_INFO(LogModule::WORKER, "Worker re-registered: " + worker_id + " (hostname=" + hostname + ", slots=" + std::to_string(slots) + ")");
        return true;
    }

    // New registration
    auto worker = std::make_unique<Worker>(worker_id, hostname, slots);
    worker->setIdentity(machine, slot, !machine_id.empty() && !slot_id.empty());
    worker->control_=control;
    worker->setOnline();
    worker->updateHeartbeat();
    workers_[worker_id] = std::move(worker);
    ++generation_;
    LOG_INFO(LogModule::WORKER, "New worker registered: " + worker_id + " (hostname=" + hostname + ", slots=" + std::to_string(slots) + ")");
    return true;
}

bool WorkerManager::markDraining(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return false;
    it->second->setDraining();
    ++generation_;
    return true;
}

bool WorkerManager::markOffline(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return false;
    it->second->setOffline();
    ++generation_;
    return true;
}

bool WorkerManager::heartbeat(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    it->second->updateHeartbeat();
    ++generation_;
    // Heartbeats from a draining slot retain DRAINING until its connection closes.
    LOG_DEBUG(LogModule::WORKER, "Worker heartbeat: " + worker_id);
    return true;
}

bool WorkerManager::acquireSlot(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshPoliciesLocked();

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    if (it->second->getStatus() != WorkerStatus::ONLINE || !it->second->acceptsTasks() || !admitsLocked(*it->second)) {
        return false;
    }

    const bool changed = it->second->acquireSlot();
    if (changed) ++generation_;
    return changed;
}

bool WorkerManager::releaseSlot(const WorkerId& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return releaseSlotLocked(worker_id);
}

bool WorkerManager::releaseSlotLocked(const WorkerId& worker_id) {
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    const bool changed = it->second->releaseSlot();
    if (changed) ++generation_;
    return changed;
}

std::optional<Worker> WorkerManager::getWorker(const WorkerId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(id);
    if (it == workers_.end()) {
        return std::nullopt;
    }

    return *it->second;
}

std::vector<Worker> WorkerManager::getWorkers() const {
    return snapshot().second;
}

std::pair<uint64_t, std::vector<Worker>> WorkerManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Worker> result;
    result.reserve(workers_.size());

    for (const auto& [id, worker_ptr] : workers_) {
        result.push_back(*worker_ptr);
        result.back().run_policy_=policyViewLocked(*worker_ptr);
    }

    return {generation_, std::move(result)};
}

std::vector<Worker> WorkerManager::getOnlineWorkers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Worker> result;

    for (const auto& [id, worker_ptr] : workers_) {
        if (worker_ptr->getStatus() == WorkerStatus::ONLINE) {
            result.push_back(*worker_ptr);
        }
    }

    return result;
}

std::vector<Worker> WorkerManager::getAvailableWorkers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Worker> result;

    for (const auto& [id, worker_ptr] : workers_) {
        if (worker_ptr->getStatus() == WorkerStatus::ONLINE && worker_ptr->acceptsTasks() && worker_ptr->hasFreeSlot() && admitsLocked(*worker_ptr)) {
            result.push_back(*worker_ptr);
        }
    }

    return result;
}

void WorkerManager::checkTimeouts(std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    int timed_out_count = 0;

    for (auto& [id, worker_ptr] : workers_) {
        if (worker_ptr->getStatus() == WorkerStatus::OFFLINE) {
            continue;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - worker_ptr->getLastHeartbeat()
        );

        if (elapsed > timeout) {
            worker_ptr->setOffline();
            ++generation_;
            LOG_WARN(LogModule::WORKER, "Worker heartbeat timeout: " + id + " (" + std::to_string(elapsed.count()) + "s ago)");
            timed_out_count++;
        }
    }

    if (timed_out_count > 0) {
        LOG_INFO(LogModule::WORKER, "Marked " + std::to_string(timed_out_count) + " workers as offline due to heartbeat timeout");
    }
}

} // namespace GCRP
