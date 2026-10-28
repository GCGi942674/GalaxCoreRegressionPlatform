#ifndef THREADPOOL_H_
#define THREADPOOL_H_
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    ThreadPool(size_t thread_num = 8);
    ~ThreadPool();

    bool addTask(std::function<void()> task);

    void stop();
    void shutdown();

private:
    void worker();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    std::mutex mutex_;
    std::condition_variable cond_var_;
    bool stop_;
    bool accepting_;
};
#endif