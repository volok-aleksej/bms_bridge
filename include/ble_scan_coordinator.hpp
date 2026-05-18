#pragma once

#include <deque>
#include <functional>
#include <mutex>

// Serialises the scan + connect phase across all BleClients that share the
// single HCI adapter. BlueZ scanning is adapter-global (one client's
// scan_stop() kills another's discovery) and parallel connects race, so
// only one owner may scan/connect at a time; the rest wait FIFO. Already-
// established GATT connections are independent of the slot.
class BleScanCoordinator {
public:
    static BleScanCoordinator& instance();

    // Ask for the exclusive slot. If it is free, `on_granted` is invoked
    // before request() returns; otherwise the owner is queued and the
    // callback fires later (from another owner's release(), on that
    // thread). Re-requesting while already holder/queued is a no-op.
    void request(const void* owner, std::function<void()> on_granted);
    void release(const void* owner);

private:
    BleScanCoordinator() = default;

    struct Waiter {
        const void*           owner;
        std::function<void()> cb;
    };

    std::mutex         mtx_;
    const void*        holder_ = nullptr;
    std::deque<Waiter> waiters_;
};
