#include "thread.hpp"

#include <spdlog/spdlog.h>

#include <exception>

Thread::~Thread() {
    if (t_.joinable()) {
        spdlog::warn("Thread destroyed without stop(); forcing shutdown");
        stop();
    }
}

void Thread::start() {
    if (started_.exchange(true)) return;
    t_ = std::thread([this] {
        try {
            run();
        } catch (const std::exception& e) {
            spdlog::critical("worker thread terminated with exception: {}", e.what());
        } catch (...) {
            spdlog::critical("worker thread terminated with unknown exception");
        }
    });
}

void Thread::stop() {
    if (stopping_.exchange(true)) {
        if (t_.joinable()) t_.join();
        return;
    }
    on_stop();
    if (t_.joinable()) t_.join();
}
