#ifndef EVENT_LOOP_H_
#define EVENT_LOOP_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/epoll.h>
#include <unordered_map>

class EventLoop {

public:
    using TimerId = uint64_t;
    using Functor = std::function<void()>;
    using TimerCallback = std::function<void()>;
    using EventCallback = std::function<void(uint32_t)>;

public:
    struct Timer {
        uint64_t id;
        uint64_t expire_ms;
        uint64_t interval_ms;
        TimerCallback cb;
        std::atomic<bool> canceled{false};
    };

    struct TimerCompare {
        bool operator()(const std::shared_ptr<Timer>& a, const std::shared_ptr<Timer>& b) const {
            return a->expire_ms > b->expire_ms;
        }
    };

public:
    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    void addFd(int fd, uint32_t events, EventCallback cb);
    void updateFd(int fd, uint32_t events);
    void removeFd(int fd);

    void queueInLoop(Functor task);
    // true commits local queue acceptance; no throwing work follows enqueue.
    bool tryQueueInLoop(Functor task);

    bool isInLoopThread() const {
        return current_loop_ == this;
    }

    TimerId runAfter(uint64_t delay_ms, TimerCallback cb);
    TimerId runEvery(uint64_t interval_ms, TimerCallback cb);
    void cancelTimer(TimerId timer_id);

private:
    void wakeup() noexcept;
    inline static thread_local EventLoop* current_loop_{nullptr};
    void doPending();
    void handleWakeUp();

    void handleExpiredTimers();
    int getPollTimeoutMs();

private:
    int epfd_;
    int wakeup_fd_;
    std::atomic<bool> quit_{false};

    std::mutex timer_mutex_;
    std::priority_queue<std::shared_ptr<Timer>, std::vector<std::shared_ptr<Timer>>, TimerCompare>
        timers_;
    std::unordered_map<TimerId, std::shared_ptr<Timer>> timer_map_;
    std::atomic<uint64_t> next_timer_id_{1};

    std::mutex mutex_;
    std::queue<Functor> pending_tasks_;
    std::unordered_map<int, EventCallback> callbacks_;
};

#endif
