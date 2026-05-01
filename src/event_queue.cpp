#include "event_queue.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>

BmsEventQueue::BmsEventQueue() {
    evfd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (evfd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
}

BmsEventQueue::~BmsEventQueue() {
    if (evfd_ >= 0) ::close(evfd_);
}

void BmsEventQueue::push(BmsEvent ev) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        q_.push_back(std::move(ev));
    }
    uint64_t one = 1;
    (void)::write(evfd_, &one, sizeof(one));
}

std::optional<BmsEvent> BmsEventQueue::try_pop() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (q_.empty()) return std::nullopt;
    BmsEvent ev = std::move(q_.front());
    q_.pop_front();
    return ev;
}

void BmsEventQueue::drain_notify() {
    uint64_t buf;
    while (::read(evfd_, &buf, sizeof(buf)) > 0) {}
}
