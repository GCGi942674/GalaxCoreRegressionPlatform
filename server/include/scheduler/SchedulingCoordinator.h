#ifndef GCRP_SCHEDULING_COORDINATOR_H
#define GCRP_SCHEDULING_COORDINATOR_H

#include "scheduler/Scheduler.h"
#include "worker/WorkerDispatcher.h"
#include <cstdint>

namespace GCRP {

struct SchedulingResult {
    size_t assigned_count{0};
    size_t dispatched_count{0};
    size_t dispatch_failed_count{0};
    size_t rollback_failed_count{0};
};

class SchedulingCoordinator {
public:
    SchedulingCoordinator(Scheduler& scheduler, WorkerDispatcher& dispatcher);

    SchedulingCoordinator(const SchedulingCoordinator&) = delete;
    SchedulingCoordinator& operator=(const SchedulingCoordinator&) = delete;

    SchedulingResult runOnce();

private:
    Scheduler& scheduler_;
    WorkerDispatcher& dispatcher_;
};

} // namespace GCRP

#endif // GCRP_SCHEDULING_COORDINATOR_H
