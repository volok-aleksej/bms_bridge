#include "timer.hpp"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

Timer::Timer() {
    fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("timerfd_create: ") + std::strerror(errno));
    }
}

Timer::~Timer() {
    if (fd_ >= 0) ::close(fd_);
}

void Timer::arm(std::chrono::milliseconds d) {
    if (fd_ < 0) return;
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(d);
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(d - sec);
    itimerspec spec{};
    spec.it_value.tv_sec  = sec.count();
    spec.it_value.tv_nsec = ns.count();
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        spec.it_value.tv_nsec = 1;
    }
    ::timerfd_settime(fd_, 0, &spec, nullptr);
}

void Timer::arm_periodic(std::chrono::milliseconds period) {
    if (fd_ < 0) return;
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(period);
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(period - sec);
    itimerspec spec{};
    spec.it_value.tv_sec     = sec.count();
    spec.it_value.tv_nsec    = ns.count();
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        spec.it_value.tv_nsec = 1;
    }
    spec.it_interval = spec.it_value;   // repeat at the same cadence
    ::timerfd_settime(fd_, 0, &spec, nullptr);
}

void Timer::disarm() {
    if (fd_ < 0) return;
    itimerspec spec{};
    ::timerfd_settime(fd_, 0, &spec, nullptr);
}

void Timer::consume() {
    if (fd_ < 0) return;
    uint64_t v;
    (void)::read(fd_, &v, sizeof(v));
}
