#include "event.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

Event::Event() {
    fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
    }
}

Event::~Event() {
    if (fd_ >= 0) ::close(fd_);
}

void Event::signal() {
    if (fd_ < 0) return;
    uint64_t one = 1;
    (void)::write(fd_, &one, sizeof(one));
}

void Event::consume() {
    if (fd_ < 0) return;
    uint64_t v;
    (void)::read(fd_, &v, sizeof(v));
}
