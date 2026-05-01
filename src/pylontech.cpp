#include "pylontech.hpp"

#include "shared_state.hpp"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint8_t kSOI = 0x7E;
constexpr uint8_t kEOI = 0x0D;
constexpr size_t  kMinBodyLen = 16;       // VER+ADR+CID1+CID2+LENGTH+CHKSUM in ASCII
constexpr size_t  kMaxFrameLen = 4096;    // sanity cap

inline char hex_char(uint8_t v) {
    return v < 10 ? char('0' + v) : char('A' + (v - 10));
}

inline int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

inline void put_hex8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(static_cast<uint8_t>(hex_char(v >> 4)));
    out.push_back(static_cast<uint8_t>(hex_char(v & 0xF)));
}

inline void put_hex16_be(std::vector<uint8_t>& out, uint16_t v) {
    put_hex8(out, static_cast<uint8_t>(v >> 8));
    put_hex8(out, static_cast<uint8_t>(v & 0xFF));
}

uint16_t length_field(uint16_t lenid) {
    uint8_t s = ((lenid >> 8) & 0xF) + ((lenid >> 4) & 0xF) + (lenid & 0xF);
    uint8_t lchksum = static_cast<uint8_t>((~s + 1) & 0xF);
    return static_cast<uint16_t>((lchksum << 12) | (lenid & 0x0FFF));
}

uint16_t chksum_over(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += p[i];
    return static_cast<uint16_t>(~static_cast<uint16_t>(sum) + 1);
}

}

int try_parse_pylontech(const uint8_t* buf, size_t len, PylontechFrame& out) {
    if (len == 0) return 0;

    // 1. Locate SOI;
    size_t soi = 0;
    while (soi < len && buf[soi] != kSOI) ++soi;
    if (soi == len) return -static_cast<int>(len);
    if (soi > 0)    return -static_cast<int>(soi);

    // 2. Find EOI;
    size_t eoi = 1;
    while (eoi < len && buf[eoi] != kEOI) ++eoi;
    if (eoi == len) {
        if (len > kMaxFrameLen) return -1;
        return 0;
    }

    const size_t body_len = eoi - 1;
    if (body_len < kMinBodyLen || (body_len % 2) != 0) {
        return -static_cast<int>(eoi + 1);
    }

    auto decode_byte = [&](size_t pair_idx, int& v) -> bool {
        int hi = hex_val(buf[1 + pair_idx * 2]);
        int lo = hex_val(buf[1 + pair_idx * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        v = (hi << 4) | lo;
        return true;
    };

    int ver, adr, cid1, cid2, len_hi, len_lo;
    if (!decode_byte(0, ver) || !decode_byte(1, adr) ||
        !decode_byte(2, cid1) || !decode_byte(3, cid2) ||
        !decode_byte(4, len_hi) || !decode_byte(5, len_lo)) {
        return -static_cast<int>(eoi + 1);
    }

    // INFO ASCII length is whatever lies between the LENGTH field and CHKSUM,
    // regardless of what LENID claims (the inverter is known to send LENGTH=E002
    // even when INFO is empty for command 0x47).
    const size_t info_chars = body_len - kMinBodyLen;
    const size_t info_bytes = info_chars / 2;
    const size_t cs_pair_idx = 6 + info_bytes;

    int cs_hi, cs_lo;
    if (!decode_byte(cs_pair_idx, cs_hi) ||
        !decode_byte(cs_pair_idx + 1, cs_lo)) {
        return -static_cast<int>(eoi + 1);
    }

    const uint16_t expected = chksum_over(buf + 1, body_len - 4);
    const uint16_t actual = static_cast<uint16_t>((cs_hi << 8) | cs_lo);
    if (expected != actual) {
        return -static_cast<int>(eoi + 1);
    }

    out.ver  = static_cast<uint8_t>(ver);
    out.adr  = static_cast<uint8_t>(adr);
    out.cid1 = static_cast<uint8_t>(cid1);
    out.cid2 = static_cast<uint8_t>(cid2);
    out.info.clear();
    out.info.reserve(info_bytes);
    for (size_t i = 0; i < info_bytes; ++i) {
        int b;
        if (!decode_byte(6 + i, b)) return -static_cast<int>(eoi + 1);
        out.info.push_back(static_cast<uint8_t>(b));
    }

    return static_cast<int>(eoi + 1);
}

std::vector<uint8_t> build_pylontech(const PylontechFrame& f) {
    std::vector<uint8_t> body;
    body.reserve(kMinBodyLen + f.info.size() * 2);

    put_hex8(body, f.ver);
    put_hex8(body, f.adr);
    put_hex8(body, f.cid1);
    put_hex8(body, f.cid2);

    const uint16_t lenid = static_cast<uint16_t>(f.info.size() * 2);
    put_hex16_be(body, length_field(lenid));

    for (uint8_t b : f.info) put_hex8(body, b);

    const uint16_t cs = chksum_over(body.data(), body.size());

    std::vector<uint8_t> out;
    out.reserve(body.size() + 6);
    out.push_back(kSOI);
    out.insert(out.end(), body.begin(), body.end());
    put_hex16_be(out, cs);
    out.push_back(kEOI);
    return out;
}

namespace {

struct BeWriter {
    std::vector<uint8_t>& out;
    explicit BeWriter(std::vector<uint8_t>& v) : out(v) {}

    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) {
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
};

}

void AnalogResponse::serialize(std::vector<uint8_t>& info) const {
    BeWriter w(info);
    w.u8(infoflag);
    w.u8(command);
    w.u8(static_cast<uint8_t>(cell_voltages_mv.size()));
    for (auto v : cell_voltages_mv) w.u16(v);
    w.u8(static_cast<uint8_t>(temperatures_k10.size()));
    for (auto t : temperatures_k10) w.u16(t);
    w.i16(current_01a);
    w.u16(module_voltage_mv);
    w.u16(remain_capacity_mah);
    w.u8(user_defined_count);
    w.u16(total_capacity_mah);
    w.u16(cycle_count);
}

void AlarmResponse::serialize(std::vector<uint8_t>& info) const {
    BeWriter w(info);
    w.u8(dataflag);
    w.u8(command);
    w.u8(static_cast<uint8_t>(cell_voltage_states.size()));
    for (auto s : cell_voltage_states) w.u8(s);
    w.u8(static_cast<uint8_t>(temperature_states.size()));
    for (auto s : temperature_states) w.u8(s);
    w.u8(charge_current_state);
    w.u8(module_voltage_state);
    w.u8(discharge_current_state);
    w.u8(status_byte_1);
    w.u8(status_byte_2);
    w.u8(status_byte_3);
    w.u8(cell_error_states_1_8);
    w.u8(cell_error_states_9_16);
}

void SystemParameterResponse::serialize(std::vector<uint8_t>& info) const {
    BeWriter w(info);
    w.u8(infoflag);
    w.u16(cell_high_voltage_limit_mv);
    w.u16(cell_low_voltage_limit_mv);
    w.u16(cell_under_voltage_limit_mv);
    w.u16(charge_high_temp_k10);
    w.u16(charge_low_temp_k10);
    w.i16(charge_current_limit_01a);
    w.u16(module_high_voltage_limit_mv);
    w.u16(module_low_voltage_limit_mv);
    w.u16(module_under_voltage_limit_mv);
    w.u16(discharge_high_temp_k10);
    w.u16(discharge_low_temp_k10);
    w.i16(discharge_current_limit_01a);
}

void ChargeManagementResponse::serialize(std::vector<uint8_t>& info) const {
    BeWriter w(info);
    w.u8(command);
    w.u16(charge_voltage_limit_mv);
    w.u16(discharge_voltage_limit_mv);
    w.i16(charge_current_limit_01a);
    w.i16(discharge_current_limit_01a);

    uint8_t status = 0;
    if (charge_enabled)                  status |= 0x80;
    if (discharge_enabled)               status |= 0x40;
    if (request_charge_immediately)      status |= 0x20;
    if (request_charge_immediately_low)  status |= 0x10;
    if (request_full_charge)             status |= 0x08;
    w.u8(status);
}

// ---- Responders --------------------------------------------------------------

namespace {

// US2000B-class layout marker — exposed in the analog response so the inverter
// recognises us as a 15-cell-per-module Pylontech.
constexpr uint8_t  kUserDefinedUS2000B = 2;

constexpr uint8_t  kAlarmStatusByte1Normal = 0x00;
constexpr uint8_t  kAlarmStatusByte2Normal = 0x0E;
constexpr uint8_t  kAlarmStatusByte3Normal = 0x00;

uint8_t echoed_cmd_value(const PylontechFrame& req) {
    return req.info.empty() ? req.adr : req.info[0];
}

PylontechFrame ok_response(const PylontechFrame& req) {
    PylontechFrame r;
    r.ver  = req.ver;
    r.adr  = req.adr;
    r.cid1 = 0x46;
    r.cid2 = 0x00;
    return r;
}

// JK reports 0.1 °C; Pylontech expects Kelvin × 10.
inline uint16_t dC_to_k10(int16_t dC) {
    return static_cast<uint16_t>(static_cast<int32_t>(dC) + 2731);
}

inline uint16_t clamp_u16(uint32_t v) {
    return static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFFu));
}

inline int16_t clamp_i16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

// Pylontech encodes capacities as u16 mAh (max 65.535 Ah). For larger packs
// (our 314 Ah JK, for example) we scale both fields by the same factor so the
// SoC ratio remain/total survives even though the absolute Ah figure shown by
// the inverter is divided.
struct ScaledCapacity { uint16_t total_mah; uint16_t remain_mah; };

constexpr uint32_t kCapU16Max = 65000;

ScaledCapacity scale_capacity_to_u16(uint32_t total_mah, uint32_t remain_mah) {
    if (total_mah <= kCapU16Max) {
        return {static_cast<uint16_t>(total_mah), static_cast<uint16_t>(remain_mah)};
    }
    const uint32_t scale = (total_mah + kCapU16Max - 1) / kCapU16Max;
    return {static_cast<uint16_t>(total_mah / scale),
            static_cast<uint16_t>(remain_mah / scale)};
}

}

uint8_t module_count_for_capacity(uint32_t total_capacity_mah) {
    if (total_capacity_mah <= kCapU16Max) return 1;
    const uint32_t n = (total_capacity_mah + kCapU16Max - 1) / kCapU16Max;
    return static_cast<uint8_t>(std::min<uint32_t>(n, 0xFFu));
}

std::vector<uint8_t> build_analog_response(const PylontechFrame& req,
                                           const JkCellInfo& cells,
                                           const JkPackInfo& pack) {
    AnalogResponse r;
    r.command            = echoed_cmd_value(req);
    r.user_defined_count = kUserDefinedUS2000B;

    r.cell_voltages_mv = cells.voltages_mv;
    r.temperatures_k10 = {
        dC_to_k10(pack.battery_temp1_dC),
        dC_to_k10(pack.battery_temp2_dC),
        dC_to_k10(pack.power_tube_temp_dC),
    };
    r.current_01a       = clamp_i16(pack.current_ma / 100);
    r.module_voltage_mv = clamp_u16(static_cast<uint32_t>(
        std::max<int32_t>(pack.voltage_mv, 0)));

    const auto cap = scale_capacity_to_u16(pack.total_capacity_mah,
                                           pack.remaining_capacity_mah);
    r.remain_capacity_mah = cap.remain_mah;
    r.total_capacity_mah  = cap.total_mah;
    r.cycle_count         = clamp_u16(pack.cycle_count);

    PylontechFrame f = ok_response(req);
    r.serialize(f.info);
    return build_pylontech(f);
}

std::vector<uint8_t> build_alarm_response(const PylontechFrame& req,
                                          const JkCellInfo& cells) {
    AlarmResponse r;
    r.command       = echoed_cmd_value(req);
    r.status_byte_1 = kAlarmStatusByte1Normal;
    r.status_byte_2 = kAlarmStatusByte2Normal;
    r.status_byte_3 = kAlarmStatusByte3Normal;

    // We don't translate JK's errors_bitmask into per-cell / per-temp alarm
    // bytes yet — that's a future mapping. Sizes still need to match the
    // analog response so the inverter sees consistent M/N counts.
    r.cell_voltage_states.assign(cells.voltages_mv.size(), 0);
    r.temperature_states.assign(3, 0);

    PylontechFrame f = ok_response(req);
    r.serialize(f.info);
    return build_pylontech(f);
}

namespace {

// JK settings → Pylontech wire-unit conversions. Module limits are derived as
// cell_count × per-cell limit; current limits stay unscaled across virtual
// modules (the multi-module trick is precisely about giving the inverter a
// total current budget that's N× per-module).
inline uint16_t module_voltage_from_cell(uint16_t cell_mv, uint8_t cell_count) {
    const uint32_t v = static_cast<uint32_t>(cell_mv) * cell_count;
    return clamp_u16(v);
}

}

std::vector<uint8_t> build_system_param_response(const PylontechFrame& req,
                                                 const JkSettings& limits) {
    SystemParameterResponse r;
    r.cell_high_voltage_limit_mv    = limits.cell_ovp_mv;
    r.cell_low_voltage_limit_mv     = limits.cell_uvpr_mv;
    r.cell_under_voltage_limit_mv   = limits.cell_uvp_mv;
    r.charge_high_temp_k10          = dC_to_k10(limits.charge_otp_dC);
    r.charge_low_temp_k10           = dC_to_k10(limits.charge_utp_dC);
    r.charge_current_limit_01a      = clamp_i16(static_cast<int32_t>(
        limits.max_charge_current_ma / 100));
    r.module_high_voltage_limit_mv  = module_voltage_from_cell(limits.cell_ovp_mv,  limits.cell_count);
    r.module_low_voltage_limit_mv   = module_voltage_from_cell(limits.cell_uvpr_mv, limits.cell_count);
    r.module_under_voltage_limit_mv = module_voltage_from_cell(limits.cell_uvp_mv,  limits.cell_count);
    r.discharge_high_temp_k10       = dC_to_k10(limits.discharge_otp_dC);
    // JK has no separate discharge low-temp protection — reuse charge UTP.
    r.discharge_low_temp_k10        = dC_to_k10(limits.charge_utp_dC);
    r.discharge_current_limit_01a   = clamp_i16(-static_cast<int32_t>(
        limits.max_discharge_current_ma / 100));

    PylontechFrame f = ok_response(req);
    r.serialize(f.info);
    return build_pylontech(f);
}

std::vector<uint8_t> build_chgmgmt_response(const PylontechFrame& req,
                                            const JkPackInfo& pack,
                                            const JkSettings& limits) {
    ChargeManagementResponse r;
    r.command                     = echoed_cmd_value(req);
    r.charge_voltage_limit_mv     = module_voltage_from_cell(limits.cell_ovp_mv,  limits.cell_count);
    r.discharge_voltage_limit_mv  = module_voltage_from_cell(limits.cell_uvpr_mv, limits.cell_count);
    r.charge_current_limit_01a    = clamp_i16(static_cast<int32_t>(
        limits.max_charge_current_ma / 100));
    r.discharge_current_limit_01a = clamp_i16(-static_cast<int32_t>(
        limits.max_discharge_current_ma / 100));
    r.charge_enabled              = pack.charging_enabled;
    r.discharge_enabled           = pack.discharging_enabled;

    PylontechFrame f = ok_response(req);
    r.serialize(f.info);
    return build_pylontech(f);
}
