#include "scheduler/SchedulingCoordinator.h"
#include "Logging.h"

namespace GCRP {
SchedulingCoordinator::SchedulingCoordinator(Scheduler& scheduler, WorkerDispatcher& dispatcher)
    : scheduler_(scheduler)
    , dispatcher_(dispatcher) {
}

SchedulingResult SchedulingCoordinator::runOnce() {
    SchedulingResult result;
    auto assignments = scheduler_.schedule();
    result.assigned_count = assignments.size();
    for (const auto& assignment : assignments) {
        std::optional<bool> accepted;
        try {
            accepted = scheduler_.dispatchAssignment(assignment, dispatcher_);
        } catch (...) {
            // Includes snapshot/claim allocation failure, not just dispatcher errors.
            accepted = false;
        }
        if (!accepted)
            continue; // Cancelled before the claim: never send.
        if (*accepted) {
            ++result.dispatched_count;
        } else {
            ++result.dispatch_failed_count;
            if (!scheduler_.rollbackAssignment(assignment.task_id))
                ++result.rollback_failed_count;
        }
        // Diagnostics must not interrupt cleanup of the remaining batch or undo acceptance.
        try {
            if (*accepted)
                LOG_INFO(LogModule::SCHEDULER,
                    "dispatch accepted task=" + std::to_string(assignment.task_id));
            else
                LOG_WARN(LogModule::SCHEDULER,
                    "dispatch failed task=" + std::to_string(assignment.task_id));
        } catch (...) {
        }
    }
    return result;
}
} // namespace GCRP
