#pragma once

#include "scheduler/Scheduler.h"
#include "nlohmann/json.hpp"
#include <optional>
#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <future>

namespace GCRP {

struct RegressionQuery {
    std::string revision;
    std::string case_path;
    std::string flow;
    std::string batch_id;
    std::string date, baseline_date, template_name, search, change, after_key;
    size_t limit{20};
    TaskId before_task_id{0};

    static std::optional<RegressionQuery> parse(const nlohmann::json& request);
};

struct DailyCacheOptions {
    bool enabled{true};
    size_t max_entries{8}, max_bytes{64 * 1024 * 1024}, max_rows{50000};
    int ttl_seconds{5}, historical_ttl_seconds{300};
};

// Read-only view over one-case Tasks. This does not infer a PJTest parent task
// or invent a business result for an infrastructure failure.
class RegressionService {
public:
    explicit RegressionService(const Scheduler& scheduler,
        bool cache_enabled = false,
        size_t max_entries = 1000,
        int ttl_seconds = 5,
        DailyCacheOptions daily = {})
        : scheduler_(scheduler)
        , cache_enabled_(cache_enabled)
        , max_entries_(max_entries)
        , ttl_seconds_(ttl_seconds)
        , daily_options_(daily) {
    }

    nlohmann::json query(const RegressionQuery& filter) const;
    nlohmann::json compareDaily(const RegressionQuery& filter) const;
    nlohmann::json cacheStats() const;

private:
    struct CacheEntry {
        nlohmann::json response;
        std::chrono::steady_clock::time_point expires_at;
        size_t bytes{0};
    };

    const Scheduler& scheduler_;
    const bool cache_enabled_;
    const size_t max_entries_;
    const int ttl_seconds_;
    mutable std::mutex cache_mutex_;
    mutable uint64_t cached_generation_{0};
    mutable std::unordered_map<std::string, CacheEntry> cache_;
    mutable uint64_t hits_{0}, misses_{0}, invalidations_{0};
    mutable size_t legacy_bytes_{0};
    static constexpr size_t legacy_budget_ = 8 * 1024 * 1024;

    struct DailySnapshot {
        nlohmann::json data;
        uint64_t generation{0};
        size_t bytes{0};
        bool active{false};
    };

    using DailyPtr = std::shared_ptr<const DailySnapshot>;

    struct DailyEntry {
        DailyPtr value;
        std::chrono::steady_clock::time_point expires, accessed;
    };

    const DailyCacheOptions daily_options_;
    mutable std::mutex daily_mutex_;
    mutable std::unordered_map<std::string, DailyEntry> daily_cache_;
    mutable std::unordered_map<std::string, std::shared_future<DailyPtr>> daily_flights_;
    mutable size_t daily_bytes_{0};
    mutable uint64_t daily_hits_{0}, daily_misses_{0}, daily_waits_{0}, daily_builds_{0},
        daily_evictions_{0}, daily_invalidations_{0}, daily_oversized_{0};
    mutable QueryMetrics query_metrics_, daily_build_metrics_;
    DailyPtr buildDailySnapshot(const RegressionQuery&) const;
    DailyPtr dailySnapshot(const RegressionQuery&) const;
};

} // namespace GCRP
