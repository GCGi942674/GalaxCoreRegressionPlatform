#ifndef GCRP_WORKER_CONNECTION_REGISTRY_H
#define GCRP_WORKER_CONNECTION_REGISTRY_H

#include "Connection.h"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>
#include <cstdint>
#include <optional>
#include <functional>

namespace GCRP {

class WorkerConnectionRegistry {
public:
    using ConnectionHandle = std::weak_ptr<Connection>;
    using ConnectionPtr = std::shared_ptr<Connection>;
    using WorkerId = std::string;
    using HandleId = uint64_t;

    WorkerConnectionRegistry();
    ~WorkerConnectionRegistry() = default;

    WorkerConnectionRegistry(const WorkerConnectionRegistry&) = delete;
    WorkerConnectionRegistry& operator=(const WorkerConnectionRegistry&) = delete;

    bool bind(const WorkerId& worker_id, const ConnectionPtr& connection);
    std::optional<HandleId> bindWithHandle(
        const WorkerId& worker_id, const ConnectionPtr& connection);
    bool isCurrent(const WorkerId& worker_id, HandleId handle_id) const;

    bool unbind(const WorkerId& worker_id,
        HandleId handle_id,
        const std::function<void()>& on_unbound = {});

    ConnectionPtr getConnection(const WorkerId& worker_id) const;

    bool hasConnection(const WorkerId& worker_id) const;

    HandleId getHandleId(const WorkerId& worker_id) const;

private:
    struct ConnectionEntry {
        ConnectionHandle handle;
        HandleId handle_id;
    };

    mutable std::mutex mutex_;
    std::unordered_map<WorkerId, ConnectionEntry> registry_;
    std::atomic<HandleId> next_handle_id_{1};
};

} // namespace GCRP

#endif // GCRP_WORKER_CONNECTION_REGISTRY_H
