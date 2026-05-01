#pragma once

#include <chrono>

class Timer {
public:
    Timer();
    ~Timer();
    Timer(const Timer&)            = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&)                 = delete;
    Timer& operator=(Timer&&)      = delete;

    int  fd() const noexcept { return fd_; }
    void arm(std::chrono::milliseconds d);
    void arm_periodic(std::chrono::milliseconds period);
    void disarm();
    void consume();

private:
    int fd_ = -1;
};
