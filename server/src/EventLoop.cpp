#include "EventLoop.h"
#include "Logging.h"
#include "Utils.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace GCRP;

EventLoop::EventLoop() : epfd_(-1), wakeup_fd_(-1), quit_(false) {
  this->epfd_ = epoll_create1(0);
  if (this->epfd_ < 0) {
    std::ostringstream oss;
    oss << "epoll_create1 failed, errno=" << errno << ", err=" << strerror(errno);
    LOG_ERROR(LogModule::NETWORK, oss.str());
    std::abort();
  }
  this->wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (this->wakeup_fd_ < 0) {
    std::ostringstream oss;
    oss << "eventfd failed, errno=" << errno << ", err=" << strerror(errno);
    LOG_ERROR(LogModule::NETWORK, oss.str());
    std::abort();
  }

  epoll_event ev{};
  ev.data.fd = this->wakeup_fd_;
  ev.events = EPOLLIN;
  if (epoll_ctl(this->epfd_, EPOLL_CTL_ADD, this->wakeup_fd_, &ev) < 0) {
    std::ostringstream oss;
    oss << "epoll_ctl add wakeup fd failed, errno=" << errno << ", err=" << strerror(errno);
    LOG_ERROR(LogModule::NETWORK, oss.str());
    std::abort();
  }
  std::ostringstream oss;
  oss << "event loop initialized, epfd=" << this->epfd_ << ", wakeup_fd=" << this->wakeup_fd_;
  LOG_INFO(LogModule::NETWORK, oss.str());
}

EventLoop::~EventLoop() {
  if (this->epfd_ != -1) {
    close(this->epfd_);
    this->epfd_ = -1;
  }
  if (this->wakeup_fd_ != -1) {
    close(this->wakeup_fd_);
    this->wakeup_fd_ = -1;
  }
}

void EventLoop::loop() {
  current_loop_ = this;
  LOG_INFO(LogModule::NETWORK, "event loop started");
  epoll_event events[1024];
  while (!this->quit_.load()) {
    int timeout_ms = this->getPollTimeoutMs();
    int nready = epoll_wait(this->epfd_, events, 1024, timeout_ms);

    if (nready < 0) {
      if (errno == EINTR) {

        continue;
      }
      std::ostringstream oss;
      oss << "epoll_wait failed, errno=" << errno << ", err=" << strerror(errno);
      LOG_ERROR(LogModule::NETWORK, oss.str());
      break;
    }

    {
      std::ostringstream oss;
      oss << "epoll_wait returned nready=" << nready;
      LOG_DEBUG(LogModule::NETWORK, oss.str());
    }

    for (int i = 0; i < nready; ++i) {
      int fd = events[i].data.fd;
      uint32_t event = events[i].events;

      if (fd == this->wakeup_fd_) {
        this->handleWakeUp();
        continue;
      }

      auto it = this->callbacks_.find(fd);
      if (it != this->callbacks_.end()) {
        auto callback = it->second;
        callback(event);
      } else {
        std::ostringstream oss;
        oss << "callback not found for fd=" << fd;
        LOG_WARN(LogModule::NETWORK, oss.str());
      }
    }

    this->handleExpiredTimers();
    this->doPending();
  }
  LOG_INFO(LogModule::NETWORK, "event loop exited");
  current_loop_ = nullptr;
}

void EventLoop::quit() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_.store(true);
  }
  wakeup();
}

void EventLoop::addFd(int fd, uint32_t events, EventCallback cb) {
  epoll_event ev{};
  ev.data.fd = fd;
  ev.events = events;
  if (epoll_ctl(this->epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    {
      std::ostringstream oss;
      oss << "add fd failed, fd=" << fd << ", errno=" << errno
                                   << ", err=" << strerror(errno);
      LOG_ERROR(LogModule::NETWORK, oss.str());
    }
    return;
  }
  this->callbacks_[fd] = cb;
  {
    std::ostringstream oss;
    oss << "fd added to epoll, fd=" << fd << ", events=" << events;
    LOG_DEBUG(LogModule::NETWORK, oss.str());
  }
}

void EventLoop::updateFd(int fd, uint32_t events) {
  epoll_event ev{};
  ev.data.fd = fd;
  ev.events = events;
  if (epoll_ctl(this->epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
    {
      std::ostringstream oss;
      oss << "update fd failed, fd=" << fd << ", errno=" << errno
                                      << ", err=" << strerror(errno);
      LOG_ERROR(LogModule::NETWORK, oss.str());
    }
    return;
  }
  {
    std::ostringstream oss;
    oss << "fd updated, fd=" << fd << ", events=" << events;
    LOG_DEBUG(LogModule::NETWORK, oss.str());
  }
}

void EventLoop::removeFd(int fd) {
  if (epoll_ctl(this->epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
    {
      std::ostringstream oss;
      oss << "remove fd failed, fd=" << fd << ", errno=" << errno
                                     << ", err=" << strerror(errno);
      LOG_WARN(LogModule::NETWORK, oss.str());
    }
  } else {
    {
      std::ostringstream oss;
      oss << "fd removed from epoll, fd=" << fd;
      LOG_DEBUG(LogModule::NETWORK, oss.str());
    }
  }
  this->callbacks_.erase(fd);
}

void EventLoop::queueInLoop(Functor task) {
  tryQueueInLoop(std::move(task));
}

bool EventLoop::tryQueueInLoop(Functor task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (quit_.load()) return false;
    pending_tasks_.push(std::move(task));
  }
  // Queue insertion is the acceptance boundary. Do not log/allocate afterwards.
  wakeup();
  return true;
}

void EventLoop::wakeup() noexcept {
  uint64_t one = 1;
  ssize_t n;
  do {
    n = ::write(wakeup_fd_, &one, sizeof(one));
  } while (n < 0 && errno == EINTR);
  // EAGAIN means an earlier wakeup is already pending.
}

EventLoop::TimerId EventLoop::runAfter(uint64_t delay_ms, TimerCallback cb) {
  auto timer = std::make_shared<Timer>();
  timer->id = next_timer_id_++;
  timer->expire_ms = getSteadyClockMs() + delay_ms;
  timer->interval_ms = 0;
  timer->cb = std::move(cb);
  timer->canceled = false;
  {
    std::lock_guard<std::mutex> lock(this->timer_mutex_);
    this->timer_map_[timer->id] = timer;
    this->timers_.push(timer);
  }

  this->queueInLoop([]() {});
  return timer->id;
}

EventLoop::TimerId EventLoop::runEvery(uint64_t interval_ms, TimerCallback cb) {
  auto timer = std::make_shared<Timer>();
  timer->id = next_timer_id_++;
  timer->expire_ms = getSteadyClockMs() + interval_ms;
  timer->interval_ms = interval_ms;
  timer->cb = std::move(cb);
  timer->canceled = false;
  {
    std::lock_guard<std::mutex> lock(this->timer_mutex_);
    this->timer_map_[timer->id] = timer;
    this->timers_.push(timer);
  }

  this->queueInLoop([]() {});
  return timer->id;
}

void EventLoop::cancelTimer(TimerId timer_id) {
  std::lock_guard<std::mutex> lock(this->timer_mutex_);
  auto it = this->timer_map_.find(timer_id);
  if (it != this->timer_map_.end()) {
    it->second->canceled = true;
    this->timer_map_.erase(it);
  }
}

void EventLoop::doPending() {
  std::queue<Functor> tasks;
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->pending_tasks_.empty()) {
      return;
    }
    this->pending_tasks_.swap(tasks);
  }

  {
    std::ostringstream oss;
    oss << "run pending tasks, count=" << tasks.size();
    LOG_DEBUG(LogModule::NETWORK, oss.str());
  }

  while (!tasks.empty()) {
    tasks.front()();
    tasks.pop();
  }
}

void EventLoop::handleWakeUp() {
  uint64_t one;
  while (true) {
    ssize_t n = read(this->wakeup_fd_, &one, sizeof(one));
    if (n > 0) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (n < 0 && (errno == EINTR)) {
      continue;
    }
    break;
  }
  LOG_DEBUG(LogModule::NETWORK, "wakeup fd drained");
}

void EventLoop::handleExpiredTimers() {
  std::vector<std::shared_ptr<Timer>> expired;
  uint64_t now = getSteadyClockMs();
  {
    std::lock_guard<std::mutex> lock(this->timer_mutex_);
    while (!this->timers_.empty()) {
      auto timer = this->timers_.top();

      if (timer->canceled) {
        this->timers_.pop();
        continue;
      }

      auto it = this->timer_map_.find(timer->id);
      if (it == this->timer_map_.end()) {
        this->timers_.pop();
        continue;
      }

      if (timer->expire_ms > now) {
        break;
      }

      this->timers_.pop();
      expired.push_back(timer);

      if (timer->interval_ms == 0) {
        this->timer_map_.erase(timer->id);
      }
    }
  }

  for (auto &timer : expired) {
    if (!timer->canceled) {
      timer->cb();
    }
    if (!timer->canceled && timer->interval_ms > 0) {
      timer->expire_ms = getSteadyClockMs() + timer->interval_ms;

      std::lock_guard<std::mutex> lock(this->timer_mutex_);
      if (this->timer_map_.find(timer->id) != this->timer_map_.end()) {
        this->timers_.push(timer);
      }
    }
  }
}

int EventLoop::getPollTimeoutMs() {
  std::lock_guard<std::mutex> lock(this->timer_mutex_);
  if (this->timers_.empty()) {
    return -1;
  }

  uint64_t now = getSteadyClockMs();

  while (!this->timers_.empty()) {
    auto timer = this->timers_.top();
    if (timer->canceled) {
      this->timers_.pop();
      continue;
    }

    if (this->timer_map_.find(timer->id) == this->timer_map_.end()) {
      this->timers_.pop();
      continue;
    }

    if (timer->expire_ms <= now) {
      return 0;
    }

    uint64_t diff = timer->expire_ms - now;
    return static_cast<int>(diff);
  }
  return -1;
}
