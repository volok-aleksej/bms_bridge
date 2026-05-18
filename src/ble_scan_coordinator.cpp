#include "ble_scan_coordinator.hpp"

#include <utility>

BleScanCoordinator& BleScanCoordinator::instance() {
    static BleScanCoordinator c;
    return c;
}

void BleScanCoordinator::request(const void* owner,
                                 std::function<void()> on_granted) {
    std::function<void()> grant;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (holder_ == owner) {
            grant = std::move(on_granted);          // already ours: re-grant
        } else if (holder_ == nullptr) {
            holder_ = owner;
            grant = std::move(on_granted);
        } else {
            for (const auto& w : waiters_)
                if (w.owner == owner) return;       // already queued
            waiters_.push_back({owner, std::move(on_granted)});
            return;
        }
    }
    if (grant) grant();                              // unlocked: only signals
}

void BleScanCoordinator::release(const void* owner) {
    std::function<void()> grant;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (holder_ != owner) {
            for (auto it = waiters_.begin(); it != waiters_.end(); ++it)
                if (it->owner == owner) { waiters_.erase(it); break; }
            return;
        }
        holder_ = nullptr;
        if (!waiters_.empty()) {
            Waiter w = std::move(waiters_.front());
            waiters_.pop_front();
            holder_ = w.owner;
            grant   = std::move(w.cb);
        }
    }
    if (grant) grant();
}
