#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread()
    : loop_(nullptr)
    , exiting_(false) {
}

EventLoopThread::~EventLoopThread() {
    this->stop();
}

EventLoop* EventLoopThread::startLoop() {
    this->thread_ = std::thread(&EventLoopThread::threadFunc, this);

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(this->mutex_);
        while (this->loop_ == nullptr) {
            this->cond_.wait(lock);
        }
        loop = this->loop_;
    }
    return loop;
}

void EventLoopThread::stop() {
    this->exiting_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (this->loop_ != nullptr) {
            this->loop_->quit();
        }
    }
    if (this->thread_.joinable()) {
        this->thread_.join();
    }
}

void EventLoopThread::threadFunc() {
    EventLoop loop;

    {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->loop_ = &loop;
        this->cond_.notify_one();
    }

    loop.loop();

    {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->loop_ = nullptr;
    }
}
