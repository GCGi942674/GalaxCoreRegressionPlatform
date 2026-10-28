#ifndef GCRP_WORKER_STATUS_H
#define GCRP_WORKER_STATUS_H

#include <string>

namespace GCRP {

enum class WorkerStatus { ONLINE, OFFLINE, DRAINING };

std::string toString(WorkerStatus status);

} // namespace GCRP

#endif // GCRP_WORKER_STATUS_H
