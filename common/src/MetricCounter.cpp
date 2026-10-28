#include "MetricCounter.h"

uint64_t MetricCounter::load() const {
    return this->value_.load(std::memory_order_relaxed);
}

void MetricCounter::inc(uint64_t n) {
    this->value_.fetch_add(n, std::memory_order_relaxed);
}

void MetricCounter::add(uint64_t n) {
    this->value_.fetch_add(n, std::memory_order_relaxed);
}

void MetricCounter::set(uint64_t value) {
    this->value_.store(value, std::memory_order_relaxed);
}

void MetricCounter::updateMax(uint64_t value) {
    uint64_t old = this->value_.load(std::memory_order_relaxed);
    while (old < value && !this->value_.compare_exchange_weak(
                              old, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
        ;
    }
}

int64_t Gauge::load() const {
    return this->value_.load(std::memory_order_relaxed);
}

void Gauge::inc(int64_t n) {
    this->value_.fetch_add(n, std::memory_order_relaxed);
}

void Gauge::dec(int64_t n) {
    this->value_.fetch_sub(n, std::memory_order_relaxed);
}

void Gauge::add(int64_t n) {
    this->value_.fetch_add(n, std::memory_order_relaxed);
}

void Gauge::sub(int64_t n) {
    this->value_.fetch_sub(n, std::memory_order_relaxed);
}

void Gauge::set(int64_t value) {
    this->value_.store(value, std::memory_order_relaxed);
}
