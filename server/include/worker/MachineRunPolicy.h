#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <string>
#include <set>

namespace GCRP {
// Called only under WorkerManager's mutex. Wall time selects the weekly plan;
// monotonic time controls telemetry expiry and hysteresis.
class MachineRunPolicy {
public:
    using Json=nlohmann::json;
    struct Tuning {int stale_seconds{30},stress_seconds{60},recover_seconds{180},step_seconds{120};
        int warn_percent{20},critical_percent{10},recover_percent{30},cpu_expand_percent{85};
        uint64_t disk_reserve_bytes{5368709120ULL};};
    explicit MachineRunPolicy(Tuning tuning):tuning_(tuning){}
    static Json defaults();
    static Json patch(const Json& previous,const Json& changes,int64_t wall_ms);
    static void validate(const Json& spec);
    static int slotNumber(const std::string& slot);
    void declareSlots(int slots){if(slots_==0)slots_=slots;}
    bool supported() const{return slots_>0;}
    void report(const Json& report,const std::string& machine,double now);
    Json evaluate(const Json& spec,int64_t wall_ms,const std::tm& local,double now);
    bool fresh(double now) const{return valid_&&now-sample_at_<=tuning_.stale_seconds;}
    int target() const{return target_;}
    int modeLimit() const{return limit_;}
    bool active() const{return enabled_;}
private:
    Tuning tuning_;
    int slots_{0},target_{0},limit_{0};
    bool valid_{false},enabled_{false},initialized_{false};
    Json telemetry_=Json::object();
    std::string agent_;
    std::set<std::string> retired_agents_;
    uint64_t sequence_{0};
    double sample_at_{-1e30},stress_at_{-1},healthy_at_{-1},step_at_{-1e30};
};
}
