#include "worker/Worker.h"
#include "worker/WorkerStatus.h"

namespace GCRP {

std::string toString(WorkerStatus status) {
    switch (status) {
        case WorkerStatus::ONLINE:
            return "ONLINE";
        case WorkerStatus::OFFLINE:
            return "OFFLINE";
        case WorkerStatus::DRAINING:
            return "DRAINING";
        default:
            return "UNKNOWN";
    }
}

Worker::Worker(const WorkerId& id, const std::string& hostname, uint32_t total_slots)
    : id_(id)
    , hostname_(hostname)
    , status_(WorkerStatus::OFFLINE)
    , total_slots_(total_slots)
    , used_slots_(0)
    , registered_at_(std::chrono::system_clock::now())
    , last_heartbeat_() {
}

uint32_t Worker::freeSlots() const {
    if (used_slots_ >= total_slots_) {
        return 0;
    }
    return total_slots_ - used_slots_;
}

bool Worker::hasFreeSlot() const {
    return freeSlots() > 0;
}

void Worker::updateHeartbeat() {
    last_heartbeat_ = std::chrono::system_clock::now();
    if (status_ != WorkerStatus::DRAINING)
        status_ = WorkerStatus::ONLINE;
}

bool Worker::acquireSlot() {
    if (!hasFreeSlot()) {
        return false;
    }
    used_slots_++;
    return true;
}

bool Worker::releaseSlot() {
    if (used_slots_ == 0) {
        return false;
    }
    used_slots_--;
    return true;
}

} // namespace GCRP
