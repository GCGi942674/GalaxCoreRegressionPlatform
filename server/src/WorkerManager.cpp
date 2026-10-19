#include "worker/WorkerManager.h"
#include "task/TaskLimits.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "Logging.h"
#include "persistence/StateStore.h"
#include <set>
#include <stdexcept>

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
            if(found->second->getUsedSlots()!=0)throw std::logic_error("maintenance cannot run while a task owns the slot");
            auto next=controls_;auto& row=next["workers"][id];
            row["state"]=phase;row["error"]=ack["error"];
            if(row!=controls_["workers"][id])commitControls(std::move(next));
        }
    }
    const auto& c=found->second->getControl();
    Json command=nullptr;
    if(c.state=="PENDING"&&found->second->getUsedSlots()==0)
        command={{"operation_id",c.operation_id},{"scope",c.scope}};
    return {{"accepting_tasks",c.available()},{"maintenance_state",c.state},{"command",command}};
}

bool WorkerManager::registerWorker(
    const WorkerId& worker_id,
    const std::string& hostname,
    uint32_t slots,
    const std::string& machine_id,
    const std::string& slot_id
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

    if (slots == 0 || (!machine_id.empty() && machine_id.size() > 64) ||
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

    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        return false;
    }

    if (it->second->getStatus() != WorkerStatus::ONLINE || !it->second->acceptsTasks()) {
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
        if (worker_ptr->getStatus() == WorkerStatus::ONLINE && worker_ptr->acceptsTasks() && worker_ptr->hasFreeSlot()) {
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
