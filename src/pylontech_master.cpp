#include "pylontech_master.hpp"

#include "pylontech.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>

namespace {

// Pylontech temperatures are Kelvin × 10; JK uses 0.1 °C.
constexpr int kKelvinOffset_dK = 2731;

int16_t k10_to_dC(uint16_t k10) {
    return static_cast<int16_t>(static_cast<int>(k10) - kKelvinOffset_dK);
}

void fill_cell_stats(JkCellInfo& c) {
    if (c.voltages_mv.empty()) return;
    uint32_t sum = 0;
    uint16_t lo = c.voltages_mv[0], hi = c.voltages_mv[0];
    uint8_t  lo_idx = 0, hi_idx = 0;
    for (size_t i = 0; i < c.voltages_mv.size(); ++i) {
        const uint16_t v = c.voltages_mv[i];
        sum += v;
        if (v > hi) { hi = v; hi_idx = static_cast<uint8_t>(i); }
        if (v < lo) { lo = v; lo_idx = static_cast<uint8_t>(i); }
    }
    c.average_voltage_mv   = static_cast<uint16_t>(sum / c.voltages_mv.size());
    c.voltage_diff_mv      = static_cast<uint16_t>(hi - lo);
    c.max_voltage_cell_idx = hi_idx;
    c.min_voltage_cell_idx = lo_idx;
}

}  // namespace

PylontechMasterProtocol::PylontechMasterProtocol(uint8_t wire_adr)
    : wire_adr_(wire_adr) {}

void PylontechMasterProtocol::reset() {
    rx_.clear();
}

std::vector<uint8_t> PylontechMasterProtocol::poll_request() {
    return build_analog_request(wire_adr_);
}

std::vector<uint8_t> PylontechMasterProtocol::settings_request() {
    return build_system_param_request(wire_adr_);
}

BatteryProtocol::Parsed PylontechMasterProtocol::feed(const uint8_t* data,
                                                      size_t len) {
    Parsed out;
    rx_.insert(rx_.end(), data, data + len);

    while (!rx_.empty()) {
        PylontechFrame f;
        const int n = try_parse_pylontech(rx_.data(), rx_.size(), f);
        if (n == 0) break;                       // need more bytes
        if (n < 0) {                             // drop junk / bad frame
            size_t drop = static_cast<size_t>(-n);
            if (drop > rx_.size()) drop = rx_.size();
            rx_.erase(rx_.begin(), rx_.begin() + drop);
            continue;
        }
        rx_.erase(rx_.begin(), rx_.begin() + n);

        if (f.adr != wire_adr_) {
            spdlog::trace("pyl-master: frame adr={:#04x} (not us {:#04x})",
                          f.adr, wire_adr_);
            continue;
        }

        if (f.cid2 == 0x42) {
            AnalogResponse a;
            if (!a.deserialize(f.info)) {
                spdlog::warn("pyl-master: bad analog payload");
                continue;
            }
            Telemetry t;
            t.cells.voltages_mv = a.cell_voltages_mv;
            fill_cell_stats(t.cells);

            t.pack.voltage_mv = a.module_voltage_mv;
            t.pack.current_ma = a.current_01a * 100;
            if (a.temperatures_k10.size() > 0)
                t.pack.battery_temp1_dC   = k10_to_dC(a.temperatures_k10[0]);
            if (a.temperatures_k10.size() > 1)
                t.pack.battery_temp2_dC   = k10_to_dC(a.temperatures_k10[1]);
            if (a.temperatures_k10.size() > 2)
                t.pack.power_tube_temp_dC = k10_to_dC(a.temperatures_k10[2]);
            t.pack.remaining_capacity_mah = a.remain_capacity_mah;
            t.pack.total_capacity_mah     = a.total_capacity_mah;
            t.pack.cycle_count            = a.cycle_count;
            t.pack.state_of_charge_pct =
                a.total_capacity_mah
                    ? static_cast<uint8_t>(std::min<uint32_t>(
                          100, static_cast<uint32_t>(a.remain_capacity_mah)
                                   * 100 / a.total_capacity_mah))
                    : 0;
            // charging/discharging flags + errors come from 0x44 / 0x92 —
            // a separate follow-up; left at defaults for now.

            last_cell_count_ = static_cast<uint8_t>(a.cell_voltages_mv.size());
            out.telemetry = std::move(t);
        } else if (f.cid2 == 0x47) {
            SystemParameterResponse sp;
            if (!sp.deserialize(f.info)) {
                spdlog::warn("pyl-master: bad system-param payload");
                continue;
            }
            JkSettings s;
            s.cell_ovp_mv  = sp.cell_high_voltage_limit_mv;
            s.cell_uvpr_mv = sp.cell_low_voltage_limit_mv;
            s.cell_uvp_mv  = sp.cell_under_voltage_limit_mv;
            s.charge_otp_dC    = k10_to_dC(sp.charge_high_temp_k10);
            s.charge_utp_dC    = k10_to_dC(sp.charge_low_temp_k10);
            s.discharge_otp_dC = k10_to_dC(sp.discharge_high_temp_k10);
            s.max_charge_current_ma =
                static_cast<uint32_t>(sp.charge_current_limit_01a) * 100;
            s.max_discharge_current_ma =
                static_cast<uint32_t>(std::abs(sp.discharge_current_limit_01a))
                * 100;
            s.cell_count = last_cell_count_;
            out.settings = std::move(s);
        } else {
            spdlog::debug("pyl-master: unhandled cid2={:#04x}", f.cid2);
        }
    }
    return out;
}
