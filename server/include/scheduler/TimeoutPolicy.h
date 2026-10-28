#pragma once
#include "task/Task.h"
#include "worker/Worker.h"
#include "persistence/StateStore.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <set>
#include <vector>
#include <map>
#include <atomic>

namespace GCRP {
class TimeoutPolicy {
public:
    explicit TimeoutPolicy(StateStore& store);
    ~TimeoutPolicy();
    TimeoutPolicy(const TimeoutPolicy&) = delete;
    TimeoutPolicy& operator=(const TimeoutPolicy&) = delete;
    nlohmann::json decide(const Task& task, const Worker& worker) const;
    void completed(const Task& task) noexcept;
    void refresh();

    struct Options {
        std::string mode{"observe"}, baseline_file;
        int history_days{30}, min_success{10}, min_seconds{300}, max_seconds{5400},
            margin_seconds{60};
        double factor{2.0};
    };

private:
    StateStore& store_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_{false};
    std::atomic<bool> degraded_{false};
    std::chrono::steady_clock::time_point next_prune_{};
    std::thread thread_;
    uint64_t generation_{0};
    std::map<std::string, std::vector<nlohmann::json>> profiles_;
    std::map<std::string, std::pair<double, size_t>> stage_ratios_;
    std::set<std::string> guards_;
    nlohmann::json baselines_ = nlohmann::json::object();
    std::string baseline_error_;
};
} // namespace GCRP
