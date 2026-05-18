#pragma once

#include "battery_protocol.hpp"

#include <cstdint>
#include <vector>

// Pylontech master: the bridge polls a Pylontech-speaking battery over UART.
// Charge/discharge flags and alarms (0x44 / 0x92) are a separate follow-up.
class PylontechMasterProtocol : public BatteryProtocol {
public:
    // wire_adr is the on-the-wire module address (config value + 1).
    explicit PylontechMasterProtocol(uint8_t wire_adr);

    void reset() override;
    std::vector<uint8_t> poll_request() override;
    std::vector<uint8_t> settings_request() override;
    Parsed feed(const uint8_t* data, size_t len) override;

private:
    uint8_t              wire_adr_;
    std::vector<uint8_t> rx_;
    uint8_t              last_cell_count_ = 0;
};
