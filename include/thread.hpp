#pragma once

#include <atomic>
#include <thread>

class Thread {
public:
    Thread() = default;
    virtual ~Thread();

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Spawn the worker thread. No-op if already started.
    void start();

    // Cooperative shutdown: sets the stopping flag, calls on_stop()
    // (so the subclass can wake its loop), and joins. Idempotent.
    void stop();

protected:
    // Worker loop. Must poll stopping() and return when it becomes true.
    // Must not throw.
    virtual void run() = 0;

    // Called from stop() in the calling thread, after the stopping flag
    // has been set. Subclass uses this to nudge run() out of any blocking
    // wait (eventfd write, cv notify, etc.). Default: nothing.
    virtual void on_stop() {}

    bool stopping() const noexcept { return stopping_.load(); }

private:
    std::thread t_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
};
