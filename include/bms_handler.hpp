#pragma once

#include "battery_protocol.hpp"
#include "battery_transport.hpp"
#include "history.hpp"
#include "shared_state.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Transport-neutral battery driver: owns a BatteryProtocol strategy, drives
// the poll / settings timers, and writes the canonical telemetry into the
// per-battery SharedState and the shared History (tagged with battery_id).
class BmsHandler : public TransportEvents {
public:
    BmsHandler(std::string battery_id,
               std::unique_ptr<BatteryProtocol> protocol,
               BatteryTransport& transport,
               SharedState& state, History& history,
               std::chrono::milliseconds history_interval,
               std::chrono::milliseconds state_update_interval);

    // TransportEvents.
    void on_connected() override;
    void on_disconnected() override;
    void on_bytes(const uint8_t* data, size_t len) override;

    // Periodic timers (run on the dispatcher thread).
    void on_poll_tick();
    void on_settings_tick();

private:
    void send(const std::vector<uint8_t>& bytes, const char* what);
    void on_telemetry(JkCellInfo cells, JkPackInfo pack);

    std::string                      battery_id_;
    std::unique_ptr<BatteryProtocol> protocol_;
    BatteryTransport&                transport_;
    SharedState&                     state_;
    History&                         history_;

    std::chrono::milliseconds             history_interval_;
    std::chrono::milliseconds             state_update_interval_;
    std::chrono::steady_clock::time_point last_history_write_{};
    std::chrono::steady_clock::time_point last_state_update_{};
};
