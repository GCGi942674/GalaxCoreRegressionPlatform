#ifndef GCRP_TASK_TYPE_H
#define GCRP_TASK_TYPE_H

#include <string>

namespace GCRP {

enum class TaskType { REGRESSION, UNKNOWN };

std::string toString(TaskType type);

} // namespace GCRP

#endif // GCRP_TASK_TYPE_H
