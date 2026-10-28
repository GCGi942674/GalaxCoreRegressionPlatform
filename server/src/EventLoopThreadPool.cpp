#include "EventLoopThread.h"
#include "EventLoopThreadPool.h"

EventLoopThreadPool::EventLoopThreadPool(size_t thread_num)
    : thread_num_(thread_num)
    , next_(0)
    , started_(false) {
}

EventLoopThreadPool::~EventLoopThreadPool() {
    this->stop();
}

void EventLoopThreadPool::start() {
    if (this->started_) {
        return;
    }

    this->started_ = true;

    this->threads_.reserve(this->thread_num_);
    this->loops_.reserve(this->thread_num_);

    for (size_t i = 0; i < this->thread_num_; ++i) {
        auto t = std::make_unique<EventLoopThread>();
        EventLoop* loop = t->startLoop();
        this->loops_.push_back(loop);
        this->threads_.push_back(std::move(t));
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    if (this->loops_.empty()) {
        return nullptr;
    }

    EventLoop* loop = this->loops_[this->next_];
    this->next_ = (this->next_ + 1) % this->loops_.size();
    return loop;
}

void EventLoopThreadPool::stop() {
    for (auto& t : this->threads_) {
        if (t) {
            t->stop();
        }
    }
    this->threads_.clear();
    this->loops_.clear();
    this->started_ = false;
}
