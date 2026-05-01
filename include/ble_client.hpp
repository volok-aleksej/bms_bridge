#pragma once

#include "event.hpp"
#include "event_queue.hpp"
#include "thread.hpp"
#include "timer.hpp"

#include <simpleble/SimpleBLE.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

class BleClient : public Thread {
public:
    struct Settings {
        std::string device_uuid;
        std::string service_uuid;
        std::string char_write_uuid;
        std::string char_notify_uuid;
        int scan_timeout_ms = 10000;
        int adapter_poll_ms = 2000;
        int reconnect_backoff_ms = 5000;
    };

    BleClient(Settings s, BmsEventQueue& queue);
    ~BleClient() override;

    bool send(const uint8_t* data, size_t len);

protected:
    void run() override;
    void on_stop() override { stop_event_.signal(); }

private:
    enum class State {
        WaitAdapter,
        Scan,
        Connect,
        Online,
        Backoff,
        Stopping,
    };

    // Install a state.
    // Each state arms the timer / clears events it needs and returns;
    void enter(State s);

    void on_stop_event();
    void on_work_event();
    void on_timer_expired();

    bool probe_adapter();
    bool start_scan();
    void stop_scan();
    bool connect_to_peripheral();
    void teardown_peripheral();

    Settings cfg_;
    BmsEventQueue& queue_;

    std::mutex match_mtx_;
    std::optional<SimpleBLE::Adapter>    adapter_;
    std::optional<SimpleBLE::Peripheral> peripheral_;
    std::optional<SimpleBLE::Peripheral> scan_match_;

    Event stop_event_;
    Event work_event_;
    Timer timer_;

    int   epoll_fd_         = -1;
    bool  adapter_warned_   = false;
    State state_            = State::WaitAdapter;
};
