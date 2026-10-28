#ifndef EVENTLOOPTHREADPOOL_H_
#define EVENTLOOPTHREADPOOL_H_

#include <cstddef>
#include <memory>
#include <vector>

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(size_t thread_num);
    ~EventLoopThreadPool();

    void start();
    EventLoop* getNextLoop();
    void stop();

private:
    size_t thread_num_;
    size_t next_;
    bool started_;

    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

#endif // EVENTLOOPTHREADPOOL_H_