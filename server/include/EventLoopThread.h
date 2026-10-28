#ifndef EVENTLOOPTHREAD_H_
#define EVENTLOOPTHREAD_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class EventLoop;

class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();

    EventLoop* startLoop();

    void stop();

private:
    void threadFunc();

private:
    EventLoop* loop_;
    bool exiting_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
};

#endif // EventLoopThread_H_