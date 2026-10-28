#ifndef GCRP_TASK_STATUS_H
#define GCRP_TASK_STATUS_H

#include <string>

namespace GCRP {

enum class TaskStatus { PENDING, QUEUED, ASSIGNED, RUNNING, SUCCEEDED, FAILED, TIMEOUT, CANCELLED };

std::string toString(TaskStatus status);

} // namespace GCRP

#endif // GCRP_TASK_STATUS_H
