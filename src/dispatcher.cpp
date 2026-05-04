#include "dispatcher.hpp"

#include <spdlog/spdlog.h>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <signal.h>

#include <cstring>
#include <stdexcept>
#include <system_error>

namespace {

int make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "sigprocmask");
    }
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "signalfd");
    }
    return fd;
}

}  // namespace

Dispatcher::Dispatcher() {
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
    signalfd_ = make_signalfd();

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = signalfd_;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, signalfd_, &ev);

    ev.data.fd = wake_event_.fd();
    epoll_ctl(epfd_, EPOLL_CTL_ADD, wake_event_.fd(), &ev);
}

Dispatcher::~Dispatcher() {
    if (epfd_     >= 0) ::close(epfd_);
    if (signalfd_ >= 0) ::close(signalfd_);
}

void Dispatcher::watch(int fd, Handler on_readable) {
    handlers_.emplace(fd, std::move(on_readable));
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
    }
}

void Dispatcher::unwatch(int fd) {
    handlers_.erase(fd);
    if (epfd_ >= 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }
}

void Dispatcher::add_timer(int period_ms, Handler on_tick) {
    auto t = std::make_unique<Timer>();
    t->arm_periodic(std::chrono::milliseconds(period_ms));
    Timer* raw = t.get();
    Handler wrapper = [raw, h = std::move(on_tick)]() {
        raw->consume();
        h();
    };
    watch(raw->fd(), std::move(wrapper));
    timers_.push_back(std::move(t));
}

void Dispatcher::stop() {
    stopping_.store(true);
    wake_event_.signal();
}

void Dispatcher::run() {
    spdlog::debug("dispatcher: entering event loop");
    constexpr int kMaxEvents = 16;
    epoll_event events[kMaxEvents];

    while (!stopping_.load()) {
        int n = epoll_wait(epfd_, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "epoll_wait");
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == signalfd_) {
                signalfd_siginfo si;
                while (::read(signalfd_, &si, sizeof(si)) == sizeof(si)) {
                    spdlog::info("dispatcher: caught signal {}, shutting down", si.ssi_signo);
                    stopping_.store(true);
                }
                continue;
            }
            if (fd == wake_event_.fd()) {
                wake_event_.consume();
                continue;
            }
            auto it = handlers_.find(fd);
            if (it != handlers_.end()) {
                it->second();
            }
        }
    }
    spdlog::debug("dispatcher: event loop exited");
}
