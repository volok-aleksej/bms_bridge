#include "uart_transport.hpp"

#include "dispatcher.hpp"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace {

speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B0;
    }
}

}  // namespace

int serial_open(const std::string& device, int baud) {
    int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        spdlog::error("serial: open {} failed: {}", device,
                      std::strerror(errno));
        return -1;
    }

    speed_t speed = baud_to_speed(baud);
    if (speed == B0) {
        spdlog::error("serial: unsupported baud {}", baud);
        ::close(fd);
        return -1;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) < 0) {
        spdlog::error("serial: tcgetattr: {}", std::strerror(errno));
        ::close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        spdlog::error("serial: tcsetattr: {}", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

UartTransport::UartTransport(std::string device, int baud, int reconnect_ms)
    : device_(std::move(device)), baud_(baud), reconnect_ms_(reconnect_ms) {}

UartTransport::~UartTransport() {
    UartTransport::stop();
}

bool UartTransport::send(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(fd_, data + written, len - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        spdlog::warn("uart: write failed after {} bytes: {}", written,
                     std::strerror(errno));
        return false;
    }
    return true;
}

void UartTransport::start(Dispatcher& dispatcher, TransportEvents& sink) {
    sink_       = &sink;
    dispatcher_ = &dispatcher;
    dispatcher_->watch(reopen_timer_.fd(), [this] {
        reopen_timer_.consume();
        on_reopen_tick();
    });
    try_open();
}

void UartTransport::stop() {
    if (dispatcher_ && reopen_timer_.fd() >= 0)
        dispatcher_->unwatch(reopen_timer_.fd());
    close_fd();
    dispatcher_ = nullptr;
}

void UartTransport::try_open() {
    fd_ = serial_open(device_, baud_);
    if (fd_ < 0) {
        reopen_timer_.arm(std::chrono::milliseconds(reconnect_ms_));
        return;
    }
    spdlog::info("uart: {} opened at {} 8N1", device_, baud_);
    dispatcher_->watch(fd_, [this] { on_readable(); });
    sink_->on_connected();
}

void UartTransport::close_fd() {
    if (fd_ < 0) return;
    if (dispatcher_) dispatcher_->unwatch(fd_);
    ::close(fd_);
    fd_ = -1;
}

void UartTransport::on_readable() {
    uint8_t buf[512];
    ssize_t r;
    while ((r = ::read(fd_, buf, sizeof(buf))) > 0)
        sink_->on_bytes(buf, static_cast<size_t>(r));

    // r == 0 on a serial tty just means "no data right now" (with
    // VMIN/VTIME = 0 non-blocking) — NOT a disconnect. Only a real error
    // (the adapter unplugged → EIO/ENXIO/...) warrants a reopen.
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        spdlog::warn("uart: read error on {}: {}, reopening", device_,
                     std::strerror(errno));
        close_fd();
        sink_->on_disconnected();
        reopen_timer_.arm(std::chrono::milliseconds(reconnect_ms_));
    }
}

void UartTransport::on_reopen_tick() {
    if (fd_ >= 0) return;
    try_open();
}
