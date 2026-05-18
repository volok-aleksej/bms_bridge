#include "bms_handler.hpp"

#include <spdlog/spdlog.h>

#include <utility>

BmsHandler::BmsHandler(std::string battery_id,
                       std::unique_ptr<BatteryProtocol> protocol,
                       BatteryTransport& transport,
                       SharedState& state, History& history,
                       std::chrono::milliseconds history_interval,
                       std::chrono::milliseconds state_update_interval)
    : battery_id_(std::move(battery_id)),
      protocol_(std::move(protocol)),
      transport_(transport),
      state_(state), history_(history),
      history_interval_(history_interval),
      state_update_interval_(state_update_interval) {}

void BmsHandler::send(const std::vector<uint8_t>& bytes, const char* what) {
    if (bytes.empty()) return;  // push-only protocol
    if (!transport_.send(bytes.data(), bytes.size()))
        spdlog::trace("bms[{}]: {} send skipped (link down)", battery_id_, what);
}

void BmsHandler::on_poll_tick() {
    spdlog::trace("bms[{}]: poll tick", battery_id_);
    send(protocol_->poll_request(), "poll");
}

void BmsHandler::on_settings_tick() {
    spdlog::trace("bms[{}]: settings tick", battery_id_);
    send(protocol_->settings_request(), "settings");
}

void BmsHandler::on_connected() {
    spdlog::info("bms[{}]: connected", battery_id_);
    state_.set_connected(true);
    protocol_->reset();
    send(protocol_->settings_request(), "settings");
    send(protocol_->poll_request(), "poll");
}

void BmsHandler::on_disconnected() {
    spdlog::info("bms[{}]: disconnected", battery_id_);
    state_.set_connected(false);
    protocol_->reset();
}

void BmsHandler::on_bytes(const uint8_t* data, size_t len) {
    auto parsed = protocol_->feed(data, len);
    if (parsed.telemetry)
        on_telemetry(std::move(parsed.telemetry->cells),
                     std::move(parsed.telemetry->pack));
    if (parsed.settings)
        state_.apply_settings(std::move(*parsed.settings));
}

void BmsHandler::on_telemetry(JkCellInfo cells, JkPackInfo pack) {
    spdlog::debug("bms[{}]: V={:.3f} I={:.3f} SoC={}% cells={} cycles={} "
                  "T1={:.1f} T2={:.1f} Tmos={:.1f}",
                  battery_id_,
                  pack.voltage_mv  / 1000.0,
                  pack.current_ma  / 1000.0,
                  pack.state_of_charge_pct,
                  cells.voltages_mv.size(),
                  pack.cycle_count,
                  pack.battery_temp1_dC    / 10.0,
                  pack.battery_temp2_dC    / 10.0,
                  pack.power_tube_temp_dC  / 10.0);

    if (auto prev = state_.snapshot(); prev.pack) {
        if (prev.pack->errors_bitmask != pack.errors_bitmask)
            spdlog::info("bms[{}]: errors_bitmask {:#06x} -> {:#06x}",
                         battery_id_, prev.pack->errors_bitmask,
                         pack.errors_bitmask);
        if (prev.pack->charging_enabled != pack.charging_enabled)
            spdlog::info("bms[{}]: charge MOSFET {} -> {}", battery_id_,
                         prev.pack->charging_enabled ? "on" : "off",
                         pack.charging_enabled       ? "on" : "off");
        if (prev.pack->discharging_enabled != pack.discharging_enabled)
            spdlog::info("bms[{}]: discharge MOSFET {} -> {}", battery_id_,
                         prev.pack->discharging_enabled ? "on" : "off",
                         pack.discharging_enabled       ? "on" : "off");
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_history_write_ >= history_interval_) {
        history_.append(battery_id_, std::chrono::system_clock::now(),
                        cells, pack);
        last_history_write_ = now;
    }
    if (now - last_state_update_ >= state_update_interval_) {
        state_.apply_telemetry(std::move(cells), std::move(pack));
        last_state_update_ = now;
    }
}
