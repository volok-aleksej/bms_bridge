#pragma once

#include "battery_transport.hpp"
#include "timer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

class Dispatcher;

int serial_open(const std::string& device, int baud);

// Serial-line transport: the bridge is the master on the wire. Opens the
// port, watches its fd in the dispatcher, and on (re)open / loss reports
// on_connected / on_disconnected to the sink. Reconnects with a backoff
// timer, mirroring the inverter's UART handling.
class UartTransport : public BatteryTransport {
public:
    UartTransport(std::string device, int baud, int reconnect_ms);
    ~UartTransport() override;

    bool send(const uint8_t* data, size_t len) override;
    void start(Dispatcher& dispatcher, TransportEvents& sink) override;
    void stop() override;

private:
    void try_open();
    void close_fd();
    void on_readable();
    void on_reopen_tick();

    std::string      device_;
    int              baud_;
    int              reconnect_ms_;
    TransportEvents* sink_       = nullptr;
    Dispatcher*      dispatcher_ = nullptr;
    int              fd_         = -1;
    Timer            reopen_timer_;
};
