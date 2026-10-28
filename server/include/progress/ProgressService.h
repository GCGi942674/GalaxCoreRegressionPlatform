#pragma once

#include "scheduler/Scheduler.h"
#include "nlohmann/json.hpp"
#include <optional>
#include <string>

namespace GCRP {

struct ProgressQuery {
    std::string target;
    std::string flow;
    std::string before_revision;
    size_t limit{20};
    static std::optional<ProgressQuery> parse(const nlohmann::json& request);
};

struct RollbackQuery {
    std::string target;
    std::string flow;
    std::string from_revision;
    std::string to_revision;
    TaskId after_task_id{0};
    size_t limit{50};
    static std::optional<RollbackQuery> parse(const nlohmann::json& request);
};

// A read-only view of the latest attempt for each case+flow at each numeric
// revision. Only an explicit PASS followed by an explicit FAIL is a rollback.
class ProgressService {
public:
    explicit ProgressService(const Scheduler& scheduler)
        : scheduler_(scheduler) {
    }

    nlohmann::json queryProgress(const ProgressQuery& query) const;
    nlohmann::json queryRollbacks(const RollbackQuery& query) const;

private:
    const Scheduler& scheduler_;
};

} // namespace GCRP
