#pragma once
#include <atomic>
#include <chrono>
#include <exception>
#include <nlohmann/json.hpp>

namespace GCRP {
class QueryMetrics {
    std::atomic<uint64_t> count_{0},total_us_{0},max_us_{0},wait_us_{0},rows_{0},errors_{0};
public:
    void record(uint64_t elapsed,uint64_t wait=0,uint64_t rows=0,bool error=false) {
        ++count_;total_us_+=elapsed;wait_us_+=wait;rows_+=rows;if(error)++errors_;
        auto old=max_us_.load();while(old<elapsed&&!max_us_.compare_exchange_weak(old,elapsed)){}
    }
    nlohmann::json json() const {
        const auto count=count_.load();return {{"queries",count},{"total_us",total_us_.load()},
            {"max_us",max_us_.load()},{"mean_us",count?total_us_.load()/count:0},
            {"lock_wait_us",wait_us_.load()},{"rows_read",rows_.load()},{"errors",errors_.load()}};
    }
};
inline uint64_t queryMicros(std::chrono::steady_clock::time_point since) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-since).count());
}
class ScopedQueryMetric {
    QueryMetrics& metric_;std::chrono::steady_clock::time_point start_=std::chrono::steady_clock::now();
    int exceptions_=std::uncaught_exceptions();
public:
    explicit ScopedQueryMetric(QueryMetrics& metric):metric_(metric){}
    ~ScopedQueryMetric(){metric_.record(queryMicros(start_),0,0,std::uncaught_exceptions()>exceptions_);}
};
}
