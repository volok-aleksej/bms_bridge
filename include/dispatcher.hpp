#pragma once

#include "event.hpp"
#include "timer.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class Dispatcher {
public:
    using Handler = std::function<void()>;

    Dispatcher();
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    void watch(int fd, Handler on_readable);

    void add_timer(int period_ms, Handler on_tick);

    void run();
    void stop();

private:
    int epfd_     = -1;
    int signalfd_ = -1;
    Event wake_event_;
    std::atomic<bool> stopping_{false};
    std::map<int, Handler> handlers_;
    std::vector<std::unique_ptr<Timer>> timers_;
};
