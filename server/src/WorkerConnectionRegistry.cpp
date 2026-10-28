#include "worker/WorkerConnectionRegistry.h"
#include "Logging.h"
#include <shared_mutex>

namespace GCRP {

WorkerConnectionRegistry::WorkerConnectionRegistry() {
}

bool WorkerConnectionRegistry::bind(const WorkerId& worker_id, const ConnectionPtr& connection) {
    return bindWithHandle(worker_id, connection).has_value();
}

std::optional<WorkerConnectionRegistry::HandleId> WorkerConnectionRegistry::bindWithHandle(
    const WorkerId& worker_id, const ConnectionPtr& connection) {
    if (worker_id.empty()) {
        LOG_ERROR(LogModule::WORKER, "Cannot bind: empty worker_id");
        return std::nullopt;
    }

    if (!connection) {
        LOG_ERROR(LogModule::WORKER, "Cannot bind: null connection for worker=" + worker_id);
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    HandleId new_handle_id = next_handle_id_++;

    bool replaced = registry_.find(worker_id) != registry_.end();

    registry_[worker_id] = {connection, new_handle_id};

    if (replaced) {
        LOG_INFO(LogModule::WORKER, "Replaced connection for worker=" + worker_id);
    } else {
        LOG_INFO(LogModule::WORKER, "Bound connection for worker=" + worker_id);
    }

    return new_handle_id;
}

bool WorkerConnectionRegistry::isCurrent(const WorkerId& worker_id, HandleId handle_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(worker_id);
    return it != registry_.end() && it->second.handle_id == handle_id &&
           !it->second.handle.expired();
}

bool WorkerConnectionRegistry::unbind(
    const WorkerId& worker_id, HandleId handle_id, const std::function<void()>& on_unbound) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = registry_.find(worker_id);
    if (it == registry_.end()) {
        return false;
    }

    if (it->second.handle_id != handle_id) {
        LOG_WARN(LogModule::WORKER,
            "Stale unbind rejected for worker=" + worker_id + ", current_handle_id=" +
                std::to_string(it->second.handle_id) + ", requested=" + std::to_string(handle_id));
        return false;
    }

    // Only the current binding may update worker liveness. Callback must not
    // re-enter the registry; the host serializes registration and close.
    if (on_unbound)
        on_unbound();
    registry_.erase(it);
    LOG_INFO(LogModule::WORKER, "Unbound connection for worker=" + worker_id);
    return true;
}

WorkerConnectionRegistry::ConnectionPtr WorkerConnectionRegistry::getConnection(
    const WorkerId& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = registry_.find(worker_id);
    if (it == registry_.end()) {
        return nullptr;
    }

    return it->second.handle.lock();
}

bool WorkerConnectionRegistry::hasConnection(const WorkerId& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = registry_.find(worker_id);
    if (it == registry_.end()) {
        return false;
    }

    return !it->second.handle.expired();
}

WorkerConnectionRegistry::HandleId WorkerConnectionRegistry::getHandleId(
    const WorkerId& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = registry_.find(worker_id);
    if (it == registry_.end()) {
        return 0;
    }

    return it->second.handle_id;
}

} // namespace GCRP
