#pragma once

#include "jk_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Protocol-specific byte handling, transport-agnostic.
class BatteryProtocol {
public:
    virtual ~BatteryProtocol() = default;

    struct Telemetry {
        JkCellInfo cells;
        JkPackInfo pack;
    };

    struct Parsed {
        std::optional<Telemetry> telemetry;
        std::optional<JkSettings> settings;
    };

    virtual void reset() = 0;

    // Bytes to send for a periodic telemetry poll.
    // Empty if the protocol is push-only and needs no solicitation.
    virtual std::vector<uint8_t> poll_request() = 0;

    // Bytes to send for a periodic settings / device-info refresh.
    // May be empty.
    virtual std::vector<uint8_t> settings_request() = 0;

    // Feed received bytes through the internal frame assembler and return
    // whatever completed (telemetry and/or settings).
    virtual Parsed feed(const uint8_t* data, size_t len) = 0;
};
