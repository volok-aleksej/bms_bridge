#pragma once

#include "timer.hpp"
#include "battery_protocol.hpp"
#include "battery_transport.hpp"
#include "jk_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class SharedState;

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

class Dispatcher;

// One address on the parallel RS485 link of a JK-PB master BMS. Answers the
// master's polls with JK frames built from the data of another battery, so
// the master counts that battery as one of its parallel packs.
class JkRs485Slave : public TransportEvents {
public:
    JkRs485Slave(std::string name, uint8_t address,
                    const SharedState& source, BatteryTransport& transport);

    void start(Dispatcher& dispatcher);
    void stop();

    void on_connected() override;
    void on_disconnected() override;
    void on_bytes(const uint8_t* data, size_t len) override;

private:
    void on_reply_tick();

    std::string          name_;
    uint8_t              address_;
    const SharedState&   source_;
    BatteryTransport&    transport_;
    std::vector<uint8_t> rx_buf_;

    // The answer goes out a little after the poll: the master needs time to
    // turn its RS485 driver around and misses an answer that comes at once.
    Dispatcher*          dispatcher_ = nullptr;
    Timer                reply_timer_;
    std::vector<uint8_t> reply_;
};
