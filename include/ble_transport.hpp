#pragma once

#include "battery_transport.hpp"
#include "ble_client.hpp"
#include "event_queue.hpp"

#include <memory>

class BleTransport : public BatteryTransport {
public:
    explicit BleTransport(BleClient::Settings settings);
    ~BleTransport() override;

    bool send(const uint8_t* data, size_t len) override;
    void start(Dispatcher& dispatcher, TransportEvents& sink) override;
    void stop() override;

private:
    void pump();

    BmsEventQueue           queue_;
    BleClient               ble_;
    TransportEvents*        sink_       = nullptr;
    Dispatcher*             dispatcher_ = nullptr;
};
