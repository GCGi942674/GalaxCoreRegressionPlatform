#include "worker/MachineRunPolicy.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace GCRP {
namespace {
using Json=nlohmann::json;
int integer(const Json& j,const char* key,int low,int high){
    const auto& v=j.at(key);if(!v.is_number_integer()||v<low||v>high)throw std::invalid_argument(std::string("invalid policy ")+key);return v.get<int>();
}
bool ratio(const Json& j,const char* key){return j.contains(key)&&j[key].is_number()&&std::isfinite(j[key].get<double>())&&j[key]>=0&&j[key]<=100;}
}
MachineRunPolicy::Json MachineRunPolicy::defaults(){return {{"enabled",true},{"adaptive",true},{"mode","auto"},{"override_until_ms",int64_t(0)},
    {"workdays",{1,2,3,4,5}},{"day_start_minute",540},{"day_end_minute",1140},{"half_slots",0}};}
void MachineRunPolicy::validate(const Json& s){
    if(!s.is_object()||s.size()!=8||!s.at("enabled").is_boolean()||!s.at("adaptive").is_boolean()||!s.at("mode").is_string())throw std::invalid_argument("invalid machine policy");
    const auto mode=s["mode"].get<std::string>();if(mode!="auto"&&mode!="half"&&mode!="full")throw std::invalid_argument("invalid policy mode");
    if(!s.at("override_until_ms").is_number_integer()||s["override_until_ms"]<0)throw std::invalid_argument("invalid policy expiry");
    const int start=integer(s,"day_start_minute",0,1439),end=integer(s,"day_end_minute",1,1439);
    if(start>=end)throw std::invalid_argument("daytime start must precede end");integer(s,"half_slots",0,128);
    const auto& days=s.at("workdays");if(!days.is_array()||days.size()>7)throw std::invalid_argument("invalid workdays");std::set<int> seen;
    for(const auto& day:days){if(!day.is_number_integer()||day<1||day>7||!seen.insert(day.get<int>()).second)throw std::invalid_argument("invalid workdays");}
}
MachineRunPolicy::Json MachineRunPolicy::patch(const Json& previous,const Json& changes,int64_t wall_ms){
    if(!changes.is_object()||changes.empty())throw std::invalid_argument("empty machine policy");
    Json next=previous;const std::set<std::string> allowed{"enabled","adaptive","mode","duration_minutes","workdays","day_start_minute","day_end_minute","half_slots"};
    for(auto it=changes.begin();it!=changes.end();++it){if(!allowed.count(it.key()))throw std::invalid_argument("unknown policy field");if(it.key()!="duration_minutes")next[it.key()]=it.value();}
    if(changes.contains("duration_minutes")&&!changes.contains("mode"))throw std::invalid_argument("duration requires mode");
    const int duration=changes.contains("duration_minutes")?integer(changes,"duration_minutes",1,10080):240;
    if(changes.contains("mode")){
        if(changes["mode"]=="auto")next["override_until_ms"]=int64_t(0);
        else next["override_until_ms"]=wall_ms+int64_t(duration)*60000;
    }
    validate(next);return next;
}
int MachineRunPolicy::slotNumber(const std::string& slot){
    if(slot.size()<5||slot.substr(0,4)!="slot"||slot[4]=='0')return 0;int value=0;
    for(size_t i=4;i<slot.size();++i){if(slot[i]<'0'||slot[i]>'9')return 0;value=value*10+slot[i]-'0';if(value>128)return 0;}return value;
}
void MachineRunPolicy::report(const Json& r,const std::string& machine,double now){
    const std::set<std::string> keys{"version","machine_id","slot_count","agent_id","sequence","valid","age_ms","cpu_percent","mem_total_bytes","mem_available_bytes","swap_in_bytes_per_sec","swap_out_bytes_per_sec","disk_free_bytes","disk_total_bytes","psi_memory_some_avg60","psi_io_some_avg60"};
    if(!r.is_object())throw std::invalid_argument("invalid resource report");
    for(auto it=r.begin();it!=r.end();++it)if(!keys.count(it.key()))throw std::invalid_argument("unknown resource report field");
    if(!r.is_object()||r.value("version",0)!=1||r.value("machine_id",std::string())!=machine||!r.contains("valid")||!r["valid"].is_boolean())throw std::invalid_argument("invalid resource report");
    const int count=integer(r,"slot_count",1,128);
    if(!r["valid"].get<bool>()){valid_=false;telemetry_={{"valid",false}};return;}
    const auto agent=r.at("agent_id").get<std::string>();if(agent.size()!=32||agent.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::invalid_argument("invalid resource agent");
    if(retired_agents_.count(agent))return;
    if(!r.at("sequence").is_number_unsigned()||!r.at("age_ms").is_number_integer()||r["age_ms"]<0||r["age_ms"]>86400000)throw std::invalid_argument("invalid resource sample age");
    const auto sequence=r["sequence"].get<uint64_t>();
    if(agent==agent_&&sequence<=sequence_)return; // A replay cannot renew freshness or reverse a scale decision.
    const double at=now-r["age_ms"].get<double>()/1000.0;if(at<sample_at_-1)return;
    if(r["valid"].get<bool>()){
        for(const char* key:{"mem_total_bytes","mem_available_bytes","disk_total_bytes","disk_free_bytes"})
            if(!r.at(key).is_number_unsigned()||r[key]>uint64_t(1ULL<<60))throw std::invalid_argument("invalid resource bytes");
        if(r["mem_total_bytes"]==0||r["mem_available_bytes"]>r["mem_total_bytes"]||r["disk_total_bytes"]==0||r["disk_free_bytes"]>r["disk_total_bytes"])throw std::invalid_argument("invalid resource totals");
        if(!r.at("cpu_percent").is_null()&&!ratio(r,"cpu_percent"))throw std::invalid_argument("invalid CPU sample");
        for(const char* key:{"swap_in_bytes_per_sec","swap_out_bytes_per_sec"})if(!r.at(key).is_number()||!std::isfinite(r[key].get<double>())||r[key]<0)throw std::invalid_argument("invalid swap rate");
        for(const char* key:{"psi_memory_some_avg60","psi_io_some_avg60"})if(r.contains(key)&&!r[key].is_null()&&!ratio(r,key))throw std::invalid_argument("invalid PSI sample");
    }
    if(!agent_.empty()&&agent_!=agent){
        if(retired_agents_.size()>=4096){valid_=false;telemetry_={{"valid",false}};return;}
        retired_agents_.insert(agent_);
    }
    telemetry_=r;agent_=agent;sequence_=sequence;slots_=count;sample_at_=at;valid_=r["valid"].get<bool>();
}
MachineRunPolicy::Json MachineRunPolicy::evaluate(const Json& spec,int64_t wall_ms,const std::tm& local,double now){
    const int weekday=local.tm_wday?local.tm_wday:7,minute=local.tm_hour*60+local.tm_min;
    const bool workday=std::find(spec["workdays"].begin(),spec["workdays"].end(),weekday)!=spec["workdays"].end();
    const bool daytime=workday&&minute>=spec["day_start_minute"].get<int>()&&minute<spec["day_end_minute"].get<int>();
    const auto configured=spec["mode"].get<std::string>();const auto until=spec["override_until_ms"].get<int64_t>();
    const auto mode=configured!="auto"&&until>wall_ms?configured:(daytime?"half":"full");
    const int half=spec["half_slots"]==0?(slots_+1)/2:std::min(slots_,spec["half_slots"].get<int>());
    limit_=mode=="half"?half:slots_;enabled_=supported()&&spec["enabled"].get<bool>();
    std::string reason="normal",status="fresh";
    if(!supported()){target_=0;reason="upgrade_worker";status="legacy";}
    else if(!enabled_){target_=slots_;limit_=slots_;reason="disabled";status=!valid_?(telemetry_.empty()?"waiting":"invalid"):fresh(now)?"fresh":"stale";}
    else if(!valid_||!fresh(now)){target_=0;stress_at_=healthy_at_=-1;reason=valid_?"telemetry_stale":"telemetry_missing";status=valid_?"stale":telemetry_.empty()?"waiting":"invalid";}
    else {
        const double memory=100.0*telemetry_["mem_available_bytes"].get<double>()/telemetry_["mem_total_bytes"].get<double>();
        const bool disk=telemetry_["disk_free_bytes"].get<uint64_t>()<tuning_.disk_reserve_bytes;
        const bool swapping=telemetry_["swap_in_bytes_per_sec"].get<double>()+telemetry_["swap_out_bytes_per_sec"].get<double>()>1048576.0;
        const double memory_psi=telemetry_.value("psi_memory_some_avg60",Json(nullptr)).is_null()?0:telemetry_["psi_memory_some_avg60"].get<double>();
        const double io_psi=telemetry_.value("psi_io_some_avg60",Json(nullptr)).is_null()?0:telemetry_["psi_io_some_avg60"].get<double>();
        const bool critical=memory<tuning_.critical_percent||disk;
        const bool stress=memory<tuning_.warn_percent||(swapping&&memory<tuning_.recover_percent)||memory_psi>=5||io_psi>=20;
        const bool cpu_known=!telemetry_["cpu_percent"].is_null();
        const bool cpu_ok=cpu_known&&telemetry_["cpu_percent"].get<double>()<tuning_.cpu_expand_percent;
        if(!initialized_){initialized_=true;target_=!critical&&!stress&&memory>=tuning_.recover_percent?1:0;step_at_=now;}
        target_=std::min(target_,limit_);
        if(critical){target_=0;healthy_at_=-1;stress_at_=-1;step_at_=now;reason=disk?"disk_low":"memory_critical";}
        else if(!spec["adaptive"].get<bool>()){target_=limit_;reason="adaptive_disabled";}
        else if(stress){
            healthy_at_=-1;if(stress_at_<0)stress_at_=now;
            if(now-stress_at_>=tuning_.stress_seconds&&now-step_at_>=tuning_.step_seconds){target_=std::max(0,target_-1);step_at_=now;stress_at_=now;}
            reason=memory<tuning_.warn_percent?"memory_pressure":swapping?"swap_pressure":memory_psi>=5?"memory_stall":"io_pressure";
        }else{
            stress_at_=-1;
            // CPU usage alone prevents expansion; it does not classify productive work as a fault.
            const bool healthy=memory>=tuning_.recover_percent&&!swapping&&memory_psi<1&&io_psi<5&&cpu_ok;
            if(healthy){if(healthy_at_<0)healthy_at_=now;
                if(target_<limit_&&now-healthy_at_>=tuning_.recover_seconds&&now-step_at_>=tuning_.step_seconds){++target_;step_at_=now;healthy_at_=now;}}
            else healthy_at_=-1;
            if(target_<limit_)reason=!cpu_known?"cpu_warmup":!cpu_ok?"cpu_busy":"gradual_recovery";
        }
    }
    char zone[32]{};std::strftime(zone,sizeof(zone),"%Z",&local);
    Json plan=spec;plan.erase("mode");plan.erase("override_until_ms");
    return {{"supported",supported()},{"enabled",enabled_},{"configured_mode",configured!="auto"&&until<=wall_ms?"auto":configured},{"effective_mode",mode},
        {"mode_limit",limit_},{"target_limit",target_},{"reported_slots",slots_},{"reason",reason},{"resource_status",status},
        {"override_until_ms",until>wall_ms?until:int64_t(0)},{"server_timezone",zone},{"plan",plan},{"resources",telemetry_},
        {"resource_thresholds",{{"stale_seconds",tuning_.stale_seconds},{"memory_warn_percent",tuning_.warn_percent},
            {"memory_critical_percent",tuning_.critical_percent},{"memory_recover_percent",tuning_.recover_percent},
            {"cpu_expand_percent",tuning_.cpu_expand_percent},{"disk_reserve_bytes",tuning_.disk_reserve_bytes}}}};
}
}
