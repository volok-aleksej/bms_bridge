#pragma once

#include "battery_transport.hpp"
#include "uart_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Dispatcher;
class Battery;

// Pylontech RS485 slave: answers the inverter's polls. Each monitored
// battery occupies its own contiguous block of Pylontech module addresses
// (count derived from its CONFIGURED capacity, so the layout is stable and
// independent of live telemetry). The inverter sums the modules back into
// the real pack. A reserve battery is also exposed — it mirrors a monitored
// battery's voltage/cells/temps but reports SoC 100%, zero current and a
// 0.5C current limit from its own capacity, lifting the inverter's budget.
class Inverter : public TransportEvents {
public:
    Inverter(std::string device, int baud,
             const std::vector<std::unique_ptr<Battery>>& batteries);
    ~Inverter() override;

    Inverter(const Inverter&) = delete;
    Inverter& operator=(const Inverter&) = delete;

    // Wire the inverter into the dispatcher. Must be called once, before
    // dispatcher.run(); the dispatcher must outlive this Inverter.
    void attach(Dispatcher& d);

    // TransportEvents.
    void on_connected() override;
    void on_disconnected() override;
    void on_bytes(const uint8_t* data, size_t len) override;

private:
    void process_buffer();
    bool send_response(const std::vector<uint8_t>& bytes);

    // One Pylontech module address. `battery` is the owner; `module_count`
    // is how many modules that battery is split into. For a
    // reserve battery `mirror` points at the monitored battery it copies
    // voltage / cells / temps from; nullptr for a monitored battery.
    struct Slot {
        Battery* battery      = nullptr;
        Battery* mirror       = nullptr;
        uint8_t  module_count = 1;
    };
    void build_address_map();

    static constexpr uint8_t kBaseAddress = 2;

    const std::vector<std::unique_ptr<Battery>>& batteries_;
    std::vector<Slot> slots_;   // slots_[adr - kBaseAddress]

    std::vector<uint8_t> rx_buf_;
    UartTransport        transport_;
};
