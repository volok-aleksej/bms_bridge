#include "inverter.hpp"

#include "pylontech.hpp"

#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

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

constexpr size_t kRxBufLimit = 4096;

}

Inverter::Inverter(std::string device, int baud, uint8_t pylontech_address,
                   SharedState& state)
    : device_(std::move(device)),
      baud_(baud),
      pylontech_address_(pylontech_address),
      state_(state) {}

Inverter::~Inverter() { stop(); }

void Inverter::on_stop() {
    if (stop_evfd_ >= 0) {
        uint64_t one = 1;
        (void)::write(stop_evfd_, &one, sizeof(one));
    }
}

int Inverter::open_uart() {
    int fd = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        spdlog::error("inverter: open {} failed: {}", device_, std::strerror(errno));
        return -1;
    }

    speed_t speed = baud_to_speed(baud_);
    if (speed == B0) {
        spdlog::error("inverter: unsupported baud {}", baud_);
        ::close(fd);
        return -1;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) < 0) {
        spdlog::error("inverter: tcgetattr: {}", std::strerror(errno));
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
        spdlog::error("inverter: tcsetattr: {}", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

bool Inverter::send_response(const std::vector<uint8_t>& bytes) {
    if (uart_fd_ < 0) return false;
    size_t written = 0;
    while (written < bytes.size()) {
        ssize_t n = ::write(uart_fd_, bytes.data() + written, bytes.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        spdlog::warn("inverter: write failed after {} bytes: {}", written, std::strerror(errno));
        return false;
    }
    return true;
}

void Inverter::process_buffer() {
    while (!rx_buf_.empty()) {
        PylontechFrame f;
        int n = try_parse_pylontech(rx_buf_.data(), rx_buf_.size(), f);
        if (n == 0) return;
        if (n < 0) {
            size_t drop = static_cast<size_t>(-n);
            if (drop > rx_buf_.size()) drop = rx_buf_.size();
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + drop);
            continue;
        }
        rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + n);

        const auto snap = state_.snapshot();
        const bool have_live = state_.is_fresh(std::chrono::seconds(5))
                               && snap.cells.has_value() && snap.pack.has_value();
        if (!have_live || !snap.settings.has_value()) {
            spdlog::debug("inverter: req CID2={:#04x} ADR={:#04x} but BMS data not "
                          "ready (live={} settings={}), staying silent",
                          f.cid2, f.adr, have_live, snap.settings.has_value());
            continue;
        }

        // We claim a contiguous range of Pylontech addresses to look like a
        // multi-module rack so a pack larger than a single module's u16 mAh
        // capacity field can still be exposed. Module count is derived from
        // the BMS-reported capacity.
        const uint8_t module_count =
            module_count_for_capacity(snap.pack->total_capacity_mah);
        const uint8_t adr_lo = pylontech_address_;
        const uint8_t adr_hi = static_cast<uint8_t>(pylontech_address_
                                                    + module_count - 1);
        if (f.adr < adr_lo || f.adr > adr_hi) {
            spdlog::debug("inverter: req CID2={:#04x} ADR={:#04x} (not us, ignored)",
                          f.cid2, f.adr);
            continue;
        }

        // Per-module slice. Cells are shared as-is (voltages are global,
        // modules in parallel); pack-side aggregates (current, capacity) and
        // the configured current limits get divided by N so the inverter
        // sums them back to the real pack value.
        JkPackInfo per_module_pack = *snap.pack;
        per_module_pack.current_ma             /= module_count;
        per_module_pack.remaining_capacity_mah /= module_count;
        per_module_pack.total_capacity_mah     /= module_count;

        JkSettings per_module_limits = *snap.settings;
        per_module_limits.max_charge_current_ma    /= module_count;
        per_module_limits.max_discharge_current_ma /= module_count;

        std::vector<uint8_t> resp;
        switch (f.cid2) {
            case 0x42: resp = build_analog_response(f, *snap.cells, per_module_pack); break;
            case 0x44: resp = build_alarm_response(f, *snap.cells);                   break;
            case 0x47: resp = build_system_param_response(f, per_module_limits);      break;
            case 0x92: resp = build_chgmgmt_response(f, per_module_pack, per_module_limits); break;
            default:
                spdlog::warn("inverter: unsupported CID2={:#04x}, no response", f.cid2);
                continue;
        }

        spdlog::debug("inverter: req CID2={:#04x} ADR={:#04x} -> reply {} bytes",
                      f.cid2, f.adr, resp.size());
        spdlog::trace("inverter: tx: {:Xpn}", spdlog::to_hex(resp));
        send_response(resp);
    }
}

void Inverter::on_rx(const uint8_t* data, size_t len) {
    spdlog::trace("inverter: rx {} bytes: {:Xpn}", len, spdlog::to_hex(data, data + len));

    if (rx_buf_.size() + len > kRxBufLimit) {
        spdlog::warn("inverter: rx buffer overflow, dropping accumulated {} bytes",
                     rx_buf_.size());
        rx_buf_.clear();
    }
    rx_buf_.insert(rx_buf_.end(), data, data + len);
    process_buffer();
}

void Inverter::run() {
    spdlog::debug("inverter: thread started, device={} baud={} address={:#04x}",
                  device_, baud_, pylontech_address_);

    stop_evfd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_evfd_ < 0) {
        spdlog::error("inverter: eventfd: {}", std::strerror(errno));
        return;
    }

    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        spdlog::error("inverter: epoll_create1: {}", std::strerror(errno));
        ::close(stop_evfd_);
        stop_evfd_ = -1;
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = stop_evfd_;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, stop_evfd_, &ev);

    while (!stopping()) {
        if (uart_fd_ < 0) {
            uart_fd_ = open_uart();
            if (uart_fd_ < 0) {
                epoll_event w[1];
                (void)::epoll_wait(epfd_, w, 1, 5000);
                continue;
            }
            spdlog::info("inverter: UART {} opened at {} 8N1", device_, baud_);
            ev.events = EPOLLIN;
            ev.data.fd = uart_fd_;
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, uart_fd_, &ev);
        }

        epoll_event events[4];
        int n = ::epoll_wait(epfd_, events, 4, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("inverter: epoll_wait: {}", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == stop_evfd_) {
                uint64_t buf;
                while (::read(stop_evfd_, &buf, sizeof(buf)) > 0) {}
                continue;
            }
            if (fd == uart_fd_) {
                uint8_t buf[512];
                ssize_t r;
                while ((r = ::read(uart_fd_, buf, sizeof(buf))) > 0) {
                    on_rx(buf, static_cast<size_t>(r));
                }
                if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    spdlog::warn("inverter: read error: {}, reopening UART",
                                 std::strerror(errno));
                    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, uart_fd_, nullptr);
                    ::close(uart_fd_);
                    uart_fd_ = -1;
                }
            }
        }
    }

    if (uart_fd_ >= 0) {
        ::close(uart_fd_);
        uart_fd_ = -1;
    }
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
    if (stop_evfd_ >= 0) {
        ::close(stop_evfd_);
        stop_evfd_ = -1;
    }
    spdlog::debug("inverter: thread exiting");
}
