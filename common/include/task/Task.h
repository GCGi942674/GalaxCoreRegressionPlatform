#ifndef GCRP_TASK_H
#define GCRP_TASK_H

#include "TaskStatus.h"
#include "TaskType.h"
#include <string>
#include <chrono>
#include <cstdint>
#include "nlohmann/json.hpp"

namespace GCRP {

using TaskId = uint64_t;

class Task {
public:
    Task(TaskId id, TaskType type, const std::string& target, const std::string& revision);

    TaskId getId() const {
        return id_;
    }

    TaskType getType() const {
        return type_;
    }

    TaskStatus getStatus() const {
        return status_;
    }

    const std::string& getTarget() const {
        return target_;
    }

    const std::string& getRevision() const {
        return revision_;
    }

    const nlohmann::json& getPayload() const {
        return payload_;
    }

    const nlohmann::json& getExecutionSummary() const {
        return execution_summary_;
    }

    const std::string& getWorkerId() const {
        return worker_id_;
    }

    const std::string& getTimeoutMode() const {
        return timeout_mode_;
    }

    void setTimeoutMode(std::string value) {
        timeout_mode_ = std::move(value);
    }

    const nlohmann::json& getTimeoutDecision() const {
        return timeout_decision_;
    }

    void setTimeoutDecision(nlohmann::json value) {
        timeout_decision_ = std::move(value);
    }

    int getPriority() const {
        return priority_;
    }

    int getRetryCount() const {
        return retry_count_;
    }

    uint64_t getQueueSequence() const {
        return queue_sequence_;
    }

    const std::string& getRunId() const {
        return run_id_;
    }

    bool recoveryRequired() const {
        return recovery_required_;
    }

    const auto& getCreatedAt() const {
        return created_at_;
    }

    const auto& getStartedAt() const {
        return started_at_;
    }

    const auto& getFinishedAt() const {
        return finished_at_;
    }

    bool canTransitionTo(TaskStatus next) const;
    bool transitionTo(TaskStatus next);

    void setWorkerId(const std::string& worker_id) {
        worker_id_ = worker_id;
    }

    void clearWorkerId() {
        worker_id_.clear();
    }

    void setPriority(int priority) {
        priority_ = priority;
    }

    void setPayload(nlohmann::json payload) {
        payload_ = std::move(payload);
    }

    void setExecutionSummary(nlohmann::json& summary) noexcept {
        execution_summary_.swap(summary);
    }

    void incrementRetryCount() {
        retry_count_++;
    }

    void setQueueSequence(uint64_t value) {
        queue_sequence_ = value;
    }

    void setRunId(std::string value) {
        if (value != run_id_)
            timeout_decision_ = nullptr;
        run_id_ = std::move(value);
    }

    void setRecoveryRequired(bool value) {
        recovery_required_ = value;
    }

    void restore(TaskStatus status,
        std::string worker_id,
        int retry_count,
        nlohmann::json payload,
        nlohmann::json execution,
        uint64_t queue_sequence,
        std::string run_id,
        bool recovery_required,
        std::chrono::system_clock::time_point created,
        std::chrono::system_clock::time_point started,
        std::chrono::system_clock::time_point finished);

    Task(const Task&) = default;
    Task& operator=(const Task&) = default;
    Task(Task&&) = default;
    Task& operator=(Task&&) = default;

private:
    TaskId id_;
    TaskType type_;
    TaskStatus status_;

    std::string target_;
    std::string revision_;
    nlohmann::json payload_ = nlohmann::json::object();
    nlohmann::json execution_summary_ = nullptr;
    std::string worker_id_;
    std::string timeout_mode_{"fixed"};
    nlohmann::json timeout_decision_ = nullptr;

    int priority_;
    int retry_count_;
    uint64_t queue_sequence_{0};
    std::string run_id_;
    bool recovery_required_{false};

    std::chrono::system_clock::time_point created_at_;
    std::chrono::system_clock::time_point started_at_;
    std::chrono::system_clock::time_point finished_at_;
};

} // namespace GCRP

#endif // GCRP_TASK_H
