#pragma once

#include "battery_protocol.hpp"
#include "jk_protocol.hpp"

// JK BMS protocol over the (BLE) byte stream.
class JkProtocol : public BatteryProtocol {
public:
    void reset() override;
    std::vector<uint8_t> poll_request() override;
    std::vector<uint8_t> settings_request() override;
    Parsed feed(const uint8_t* data, size_t len) override;

private:
    JkFrameAssembler assembler_;
};
