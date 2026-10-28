#include "Thread_pool.h"
#include "Logging.h"

ThreadPool::ThreadPool(size_t thread_num)
    : stop_(false)
    , accepting_(true) {
    for (size_t i = 0; i < thread_num; ++i) {
        workers_.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool() {
    this->stop();
}

bool ThreadPool::addTask(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!this->accepting_) {
            return false;
        }
        tasks_.push(task);
    }
    cond_var_.notify_one();
    return true;
}

void ThreadPool::stop() {
    {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->stop_ = true;
        this->accepting_ = false;
    }

    this->cond_var_.notify_all();

    for (auto& t : this->workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ThreadPool::shutdown() {
    std::unique_lock<std::mutex> lock(this->mutex_);
    this->accepting_ = false;
}

void ThreadPool::worker() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_var_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });
            if (stop_ && tasks_.empty()) {
                return;
            }
            task = tasks_.front();
            tasks_.pop();
        }
        task();
    }
}
