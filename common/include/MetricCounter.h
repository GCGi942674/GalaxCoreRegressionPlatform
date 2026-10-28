#ifndef METRIC_COUNTER_H_
#define METRIC_COUNTER_H_

#include <atomic>
#include <cstdint>
#include <string>

class MetricCounter {

public:
    MetricCounter() = default;

    uint64_t load() const;

    void inc(uint64_t n = 1);
    void add(uint64_t n);
    void set(uint64_t value);
    void updateMax(uint64_t value);

private:
    std::atomic<uint64_t> value_{0};
};

class Gauge {
public:
    Gauge() = default;

    int64_t load() const;
    void inc(int64_t n = 1);
    void dec(int64_t n = 1);
    void add(int64_t n);
    void sub(int64_t n);
    void set(int64_t value);

private:
    std::atomic<int64_t> value_{0};
};

#endif