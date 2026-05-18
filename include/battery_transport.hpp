#pragma once

#include <cstddef>
#include <cstdint>

class Dispatcher;

class TransportEvents {
public:
    virtual ~TransportEvents() = default;
    virtual void on_connected() = 0;
    virtual void on_disconnected() = 0;
    virtual void on_bytes(const uint8_t* data, size_t len) = 0;
};

// The link to one battery's BMS. Owns its own mechanism (a BLE thread +
// event queue, or a UART fd), wires itself into the dispatcher event loop,
// and reports inbound activity to a TransportEvents sink.
class BatteryTransport {
public:
    virtual ~BatteryTransport() = default;

    // Outbound protocol bytes. Returns false if the link is down.
    virtual bool send(const uint8_t* data, size_t len) = 0;
    virtual void start(Dispatcher& dispatcher, TransportEvents& sink) = 0;
    virtual void stop() = 0;
};
