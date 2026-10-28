#include "scheduler/SchedulingDriver.h"
#include "scheduler/SchedulingCoordinator.h"
#include <exception>

namespace GCRP {

SchedulingDriver::SchedulingDriver(SchedulingCoordinator& coordinator)
    : coordinator_(coordinator) {
}

void SchedulingDriver::requestSchedule() {
    std::unique_lock<std::mutex> lock(mutex_);
    pending_ = true;
    if (running_)
        return;
    running_ = true;

    std::exception_ptr failure;
    do {
        pending_ = false;
        lock.unlock();
        try {
            // Results do not generate requests: failed dispatch waits for a new event.
            coordinator_.runOnce();
        } catch (...) {
            // Restore state and drain requests received during the failed pass.
            if (!failure)
                failure = std::current_exception();
        }
        lock.lock();
    } while (pending_);
    running_ = false;
    lock.unlock();
    if (failure)
        std::rethrow_exception(failure);
}

} // namespace GCRP
