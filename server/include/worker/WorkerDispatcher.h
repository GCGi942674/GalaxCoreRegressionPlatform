#ifndef GCRP_WORKER_DISPATCHER_H
#define GCRP_WORKER_DISPATCHER_H

#include "task/Task.h"
#include <string>

namespace GCRP {

using WorkerId = std::string;

class WorkerDispatcher {
public:
    virtual ~WorkerDispatcher() = default;

    virtual bool dispatch(const WorkerId& worker_id, const Task& task) = 0;
};

} // namespace GCRP

#endif // GCRP_WORKER_DISPATCHER_H
