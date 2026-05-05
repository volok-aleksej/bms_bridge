#include "bms_handler.hpp"

#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <vector>

BmsHandler::BmsHandler(BleClient& ble, BmsEventQueue& queue,
                       SharedState& state, History& history,
                       std::chrono::milliseconds history_interval,
                       std::chrono::milliseconds state_update_interval)
    : ble_(ble), queue_(queue), state_(state), history_(history),
      history_interval_(history_interval),
      state_update_interval_(state_update_interval) {}

void BmsHandler::send_request(uint8_t cmd) {
    std::vector<uint8_t> req;
    JkRequest{cmd, 0, 0}.serialize(req);
    if (!ble_.send(req.data(), req.size())) {
        spdlog::trace("bms: BLE send skipped (cmd {:#04x}, not connected)", cmd);
    }
}

void BmsHandler::on_poll_tick() {
    spdlog::trace("bms: poll tick — request cell_info");
    send_request(kJkCmdCellInfo);
}

void BmsHandler::on_settings_tick() {
    spdlog::trace("bms: settings tick — request device_info");
    send_request(kJkCmdDeviceInfo);
}

void BmsHandler::on_queue_event() {
    queue_.drain_notify();
    while (auto ev = queue_.try_pop()) {
        switch (ev->type) {
            case BmsEventType::Connected:    on_connected();         break;
            case BmsEventType::Disconnected: on_disconnected();      break;
            case BmsEventType::NotifyFrame:  on_notify_frame(*ev);   break;
        }
    }
}

void BmsHandler::on_connected() {
    spdlog::info("bms: connected");
    state_.set_connected(true);
    jk_assembler_.reset();
    send_request(kJkCmdDeviceInfo);
    send_request(kJkCmdCellInfo);
}

void BmsHandler::on_disconnected() {
    spdlog::info("bms: disconnected");
    state_.set_connected(false);
    jk_assembler_.reset();
}

void BmsHandler::on_notify_frame(const BmsEvent& ev) {
    spdlog::trace("bms: notify {} bytes: {:Xpn}",
                  ev.payload.size(), spdlog::to_hex(ev.payload));

    jk_assembler_.push_chunk(ev.payload.data(), ev.payload.size());
    while (auto frame = jk_assembler_.take_complete_frame()) {
        const auto type = jk_response_frame_type(frame->data(), frame->size());
        if (!type) continue;

        if (*type == kJkFrameTypeCellInfo) {
            JkCellInfo cells;
            JkPackInfo pack;
            if (parse_cell_info(frame->data(), frame->size(), cells, pack))
                on_cell_info(std::move(cells), std::move(pack));
        } else if (*type == kJkFrameTypeDeviceInfo) {
            on_device_info(frame->data(), frame->size());
        } else if (*type == kJkFrameTypeSettings) {
            on_settings_frame(frame->data(), frame->size());
        } else {
            spdlog::warn("bms: unknown frame type {:#04x}", *type);
        }
    }
}

void BmsHandler::on_cell_info(JkCellInfo cells, JkPackInfo pack) {
    spdlog::debug("bms: V={:.3f} I={:.3f} SoC={}% cells={} cycles={} "
                  "T1={:.1f} T2={:.1f} Tmos={:.1f}",
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
            spdlog::info("bms: errors_bitmask {:#06x} -> {:#06x}",
                         prev.pack->errors_bitmask, pack.errors_bitmask);
        if (prev.pack->charging_enabled != pack.charging_enabled)
            spdlog::info("bms: charge MOSFET {} -> {}",
                         prev.pack->charging_enabled ? "on" : "off",
                         pack.charging_enabled       ? "on" : "off");
        if (prev.pack->discharging_enabled != pack.discharging_enabled)
            spdlog::info("bms: discharge MOSFET {} -> {}",
                         prev.pack->discharging_enabled ? "on" : "off",
                         pack.discharging_enabled       ? "on" : "off");
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_history_write_ >= history_interval_) {
        history_.append(std::chrono::system_clock::now(), cells, pack);
        last_history_write_ = now;
    }
    if (now - last_state_update_ >= state_update_interval_) {
        state_.apply_telemetry(std::move(cells), std::move(pack));
        last_state_update_ = now;
    }
}

void BmsHandler::on_device_info(const uint8_t* data, size_t len) {
    JkDeviceInfo d;
    if (parse_device_info(data, len, d))
        spdlog::debug("bms: device vendor='{}' fw={} hw={} sn={}",
                      d.vendor_id, d.firmware_version,
                      d.hardware_version, d.serial_number);
}

void BmsHandler::on_settings_frame(const uint8_t* data, size_t len) {
    JkSettings s;
    if (parse_settings(data, len, s)) {
        spdlog::debug("bms: settings cells={} ovp={}mV uvp={}mV "
                      "Ichg={}mA Idis={}mA",
                      s.cell_count, s.cell_ovp_mv, s.cell_uvp_mv,
                      s.max_charge_current_ma, s.max_discharge_current_ma);
        state_.apply_settings(std::move(s));
    }
}
