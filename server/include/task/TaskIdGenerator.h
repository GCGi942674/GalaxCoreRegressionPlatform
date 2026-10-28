#ifndef GCRP_TASK_ID_GENERATOR_H
#define GCRP_TASK_ID_GENERATOR_H

#include <cstdint>
#include <atomic>

namespace GCRP {

using TaskId = uint64_t;

class TaskIdGenerator {
public:
    TaskIdGenerator() = default;
    ~TaskIdGenerator() = default;

    TaskIdGenerator(const TaskIdGenerator&) = delete;
    TaskIdGenerator& operator=(const TaskIdGenerator&) = delete;

    TaskId next();

    void setNext(TaskId value) {
        next_id_.store(value, std::memory_order_relaxed);
    }

    TaskIdGenerator(TaskIdGenerator&&) = delete;
    TaskIdGenerator& operator=(TaskIdGenerator&&) = delete;

private:
    std::atomic<TaskId> next_id_{1};
};

} // namespace GCRP

#endif // GCRP_TASK_ID_GENERATOR_H
