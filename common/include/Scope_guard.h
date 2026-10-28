#pragma once

#include <utility>
#include <type_traits>

template <typename F> class ScopeGuard {
public:
    explicit ScopeGuard(F&& f) noexcept
        : func_(std::forward<F>(f))
        , active_(true) {
    }

    ~ScopeGuard() noexcept {
        if (active_) {
            func_();
        }
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    ScopeGuard(ScopeGuard&& other) noexcept
        : func_(std::move(other.func_))
        , active_(other.active_) {
        other.active_ = false;
    }

    ScopeGuard& operator=(ScopeGuard&&) = delete;

    void dismiss() noexcept {
        active_ = false;
    }

private:
    F func_;
    bool active_;
};

template <typename F> auto finally(F&& f) {
    return ScopeGuard<std::decay_t<F>>(std::forward<F>(f));
}