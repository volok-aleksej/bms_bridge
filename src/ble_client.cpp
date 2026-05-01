#include "ble_client.hpp"

#include <spdlog/spdlog.h>

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool ids_match(const std::string& a, const std::string& b) {
    return to_lower(a) == to_lower(b);
}

}  // namespace

BleClient::BleClient(Settings s, BmsEventQueue& queue)
    : cfg_(std::move(s)), queue_(queue) {}

BleClient::~BleClient() { stop(); }

bool BleClient::send(const uint8_t* data, size_t len) {
    if (!peripheral_ || !peripheral_->is_connected()) return false;
    SimpleBLE::ByteArray payload(reinterpret_cast<const char*>(data), len);
    try {
        peripheral_->write_command(cfg_.service_uuid, cfg_.char_write_uuid, payload);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("ble: write_command failed: {}", e.what());
        return false;
    }
}

bool BleClient::probe_adapter() {
    try {
        if (SimpleBLE::Adapter::bluetooth_enabled()) {
            auto adapters = SimpleBLE::Adapter::get_adapters();
            if (!adapters.empty()) {
                adapter_ = adapters.front();
                spdlog::info("ble: adapter ready: {} [{}]",
                             adapter_->identifier(), adapter_->address());
                return true;
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("ble: adapter probe: {}", e.what());
    }
    return false;
}

bool BleClient::start_scan() {
    if (!adapter_) return false;

    {
        std::lock_guard<std::mutex> lk(match_mtx_);
        scan_match_.reset();
    }

    adapter_->set_callback_on_scan_found([this](SimpleBLE::Peripheral p) {
        bool matched = false;
        {
            std::lock_guard<std::mutex> lk(match_mtx_);
            if (!scan_match_ &&
                (ids_match(p.address(),    cfg_.device_uuid) ||
                 ids_match(p.identifier(), cfg_.device_uuid))) {
                scan_match_ = std::move(p);
                matched = true;
            }
        }
        if (matched) work_event_.signal();
    });

    spdlog::info("ble: scanning for {} (up to {} ms)",
                 cfg_.device_uuid, cfg_.scan_timeout_ms);
    try {
        adapter_->scan_start();
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("ble: scan_start failed: {}", e.what());
        adapter_->set_callback_on_scan_found(nullptr);
        adapter_.reset();
        return false;
    }
}

void BleClient::stop_scan() {
    if (!adapter_) return;
    try { adapter_->scan_stop(); } catch (...) {}
    adapter_->set_callback_on_scan_found(nullptr);
}

bool BleClient::connect_to_peripheral() {
    if (!peripheral_) return false;

    peripheral_->set_callback_on_disconnected([this] {
        work_event_.signal();
    });

    try {
        peripheral_->connect();
    } catch (const std::exception& e) {
        spdlog::warn("ble: connect failed: {}", e.what());
        peripheral_.reset();
        return false;
    }
    if (!peripheral_->is_connected()) {
        spdlog::warn("ble: peripheral did not transition to connected");
        peripheral_.reset();
        return false;
    }

    spdlog::info("ble: connected to {} [{}]",
                 peripheral_->identifier(), peripheral_->address());

    if (spdlog::should_log(spdlog::level::info)) {
        for (auto& svc : peripheral_->services()) {
            spdlog::info("ble:   service {}", svc.uuid());
            for (auto& ch : svc.characteristics()) {
                spdlog::info("ble:     char {}", ch.uuid());
            }
        }
    }

    try {
        peripheral_->notify(
            cfg_.service_uuid, cfg_.char_notify_uuid,
            [this](SimpleBLE::ByteArray payload) {
                BmsEvent ev{BmsEventType::NotifyFrame, {}};
                ev.payload.assign(payload.begin(), payload.end());
                queue_.push(std::move(ev));
            });
    } catch (const std::exception& e) {
        spdlog::warn("ble: notify subscribe failed: {}", e.what());
        try { peripheral_->disconnect(); } catch (...) {}
        peripheral_.reset();
        return false;
    }

    queue_.push({BmsEventType::Connected, {}});
    return true;
}

void BleClient::teardown_peripheral() {
    if (!peripheral_) return;
    try {
        if (peripheral_->is_connected()) peripheral_->disconnect();
    } catch (const std::exception& e) {
        spdlog::warn("ble: disconnect: {}", e.what());
    }
    peripheral_.reset();
}

void BleClient::enter(State s) {
    state_ = s;
    switch (s) {
        case State::WaitAdapter:
            if (probe_adapter()) {
                adapter_warned_ = false;
                enter(State::Scan);
            } else {
                if (!adapter_warned_) {
                    spdlog::warn("ble: no Bluetooth adapter, retrying every {} ms",
                                 cfg_.adapter_poll_ms);
                    adapter_warned_ = true;
                }
                timer_.arm(std::chrono::milliseconds(cfg_.adapter_poll_ms));
            }
            break;

        case State::Scan:
            work_event_.consume();
            if (start_scan()) {
                timer_.arm(std::chrono::milliseconds(cfg_.scan_timeout_ms));
            } else {
                enter(State::Backoff);
            }
            break;

        case State::Connect:
            timer_.disarm();
            if (connect_to_peripheral()) {
                enter(State::Online);
            } else {
                enter(State::Backoff);
            }
            break;

        case State::Online:
            work_event_.consume();
            break;

        case State::Backoff:
            timer_.arm(std::chrono::milliseconds(cfg_.reconnect_backoff_ms));
            break;

        case State::Stopping:
            timer_.disarm();
            break;
    }
}

void BleClient::on_stop_event() {
    teardown_peripheral();
    state_ = State::Stopping;
}

void BleClient::on_work_event() {
    switch (state_) {
        case State::Scan: {
            std::optional<SimpleBLE::Peripheral> found;
            {
                std::lock_guard<std::mutex> lk(match_mtx_);
                found = std::move(scan_match_);
                scan_match_.reset();
            }
            if (!found) return;
            timer_.disarm();
            stop_scan();
            peripheral_ = std::move(*found);
            enter(State::Connect);
            break;
        }
        case State::Online:
            queue_.push({BmsEventType::Disconnected, {}});
            teardown_peripheral();
            spdlog::info("ble: connection lost, reconnecting after {} ms",
                         cfg_.reconnect_backoff_ms);
            enter(State::Backoff);
            break;
        default:
            break;
    }
}

void BleClient::on_timer_expired() {
    switch (state_) {
        case State::WaitAdapter:
            enter(State::WaitAdapter);
            break;
        case State::Scan:
            stop_scan();
            spdlog::info("ble: device {} not found this round", cfg_.device_uuid);
            enter(State::Backoff);
            break;
        case State::Backoff:
            enter(adapter_ ? State::Scan : State::WaitAdapter);
            break;
        default:
            break;
    }
}

void BleClient::run() {
    spdlog::debug("ble: management thread started");

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        spdlog::error("ble: epoll_create1: {}", std::strerror(errno));
        return;
    }
    auto add = [this](int fd) {
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    };
    add(stop_event_.fd());
    add(work_event_.fd());
    add(timer_.fd());

    enter(State::WaitAdapter);

    while (state_ != State::Stopping) {
        epoll_event evs[3];
        int n = ::epoll_wait(epoll_fd_, evs, 3, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("ble: epoll_wait: {}", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n && state_ != State::Stopping; ++i) {
            const int fd = evs[i].data.fd;
            if (fd == stop_event_.fd()) {
                stop_event_.consume();
                on_stop_event();
            } else if (fd == work_event_.fd()) {
                work_event_.consume();
                on_work_event();
            } else if (fd == timer_.fd()) {
                timer_.consume();
                on_timer_expired();
            }
        }
    }

    ::close(epoll_fd_);
    epoll_fd_ = -1;
    spdlog::debug("ble: management thread exiting");
}
