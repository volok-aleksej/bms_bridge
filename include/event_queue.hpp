#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

enum class BmsEventType {
    Connected,
    Disconnected,
    NotifyFrame,
};

struct BmsEvent {
    BmsEventType type;
    std::vector<uint8_t> payload;
};

class BmsEventQueue {
public:
    BmsEventQueue();
    ~BmsEventQueue();
    BmsEventQueue(const BmsEventQueue&) = delete;
    BmsEventQueue& operator=(const BmsEventQueue&) = delete;

    void push(BmsEvent ev);
    std::optional<BmsEvent> try_pop();

    // Drain the eventfd counter (call this once after epoll_wait wakes).
    void drain_notify();

    int notify_fd() const noexcept { return evfd_; }

private:
    int evfd_ = -1;
    std::mutex mtx_;
    std::deque<BmsEvent> q_;
};
