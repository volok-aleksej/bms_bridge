#include "jk_protocol.hpp"

#include <algorithm>
#include <cstring>

namespace {

struct LeWriter {
    std::vector<uint8_t>& out;
    explicit LeWriter(std::vector<uint8_t>& v) : out(v) {}

    void u8(uint8_t v) { out.push_back(v); }
    void u32(uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >>  0));
        out.push_back(static_cast<uint8_t>(v >>  8));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 24));
    }
    void zeros(size_t n) { out.insert(out.end(), n, 0); }
    void bytes(const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); }
};

inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline int16_t le16s(const uint8_t* p) {
    return static_cast<int16_t>(le16(p));
}

inline uint32_t le32(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) <<  8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t le32s(const uint8_t* p) {
    return static_cast<int32_t>(le32(p));
}

inline void put16(std::vector<uint8_t>& f, size_t off, uint16_t v) {
    f[off]     = static_cast<uint8_t>(v);
    f[off + 1] = static_cast<uint8_t>(v >> 8);
}

inline void put32(std::vector<uint8_t>& f, size_t off, uint32_t v) {
    put16(f, off, static_cast<uint16_t>(v));
    put16(f, off + 2, static_cast<uint16_t>(v >> 16));
}

constexpr size_t kMaxCells = 32;

// Settings frame: RS485 address of the device.
constexpr size_t kSettingsDeviceAddressOffset = 270;

std::vector<uint8_t> empty_frame(uint8_t type) {
    std::vector<uint8_t> f(kJkResponseSize, 0);
    std::copy(std::begin(kJkResponseMagic), std::end(kJkResponseMagic), f.begin());
    f[4] = type;
    return f;
}

void seal_frame(std::vector<uint8_t>& f) {
    f[kJkResponseSize - 1] = jk_crc8(f.data(), kJkResponseSize - 1);
}

std::string read_string(const uint8_t* p, size_t max_len) {
    size_t real = 0;
    while (real < max_len && p[real] != 0) ++real;
    std::string s(reinterpret_cast<const char*>(p), real);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool starts_with_response_magic(const uint8_t* p, size_t len) {
    return len >= 4 &&
           p[0] == kJkResponseMagic[0] &&
           p[1] == kJkResponseMagic[1] &&
           p[2] == kJkResponseMagic[2] &&
           p[3] == kJkResponseMagic[3];
}

}

void JkRequest::serialize(std::vector<uint8_t>& out) const {
    out.reserve(out.size() + kJkRequestSize);
    const size_t start = out.size();
    LeWriter w(out);

    w.bytes(kJkRequestMagic, 4);
    w.u8(command);
    w.u8(value_length);
    w.u32(value);
    w.zeros(9);

    const uint8_t crc = jk_crc8(out.data() + start, kJkRequestSize - 1);
    w.u8(crc);
}

uint8_t jk_crc8(const uint8_t* data, size_t len) {
    uint16_t s = 0;
    for (size_t i = 0; i < len; ++i) s += data[i];
    return static_cast<uint8_t>(s & 0xFF);
}

std::optional<uint8_t> jk_response_frame_type(const uint8_t* frame, size_t len) {
    if (len < kJkResponseSize) return std::nullopt;
    if (!starts_with_response_magic(frame, len)) return std::nullopt;
    return frame[4];
}

void JkFrameAssembler::push_chunk(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];

        // Try to align on response magic.
        if (buf_.empty() && b != kJkResponseMagic[0]) {
            continue;
        }

        buf_.push_back(b);

        if (buf_.size() <= 4) {
            const size_t pos = buf_.size() - 1;
            if (buf_[pos] != kJkResponseMagic[pos]) {
                buf_.clear();
                continue;
            }
        }

        if (buf_.size() == kJkResponseSize) {
            const uint8_t computed = jk_crc8(buf_.data(), kJkResponseSize - 1);
            const uint8_t received = buf_[kJkResponseSize - 1];
            if (computed == received) {
                ready_ = std::move(buf_);
            }
            buf_.clear();
        }
    }
}

std::optional<std::vector<uint8_t>> JkFrameAssembler::take_complete_frame() {
    if (ready_.empty()) return std::nullopt;
    std::vector<uint8_t> out = std::move(ready_);
    ready_.clear();
    return out;
}

void JkFrameAssembler::reset() {
    buf_.clear();
    ready_.clear();
}

// ---- parse_cell_info (auto-detects JK02_24S vs JK02_32S) -----------------
//
// JK firmware ships in two on-wire variants. Both keep the frame at 300 bytes,
// but the 32S variant extends the cell-voltage table by 8 slots (16 bytes) and
// shifts every following field. We detect by looking at byte 54: in 24S that's
// the cell-enabled bitmask `(1<<N)-1`; in 32S those bytes belong to cell
// voltages 24-25 (zero for short packs, mV-shaped for fully-populated 32S).
//
// One cell_info frame carries two logical groups so we fill both structs.
bool parse_cell_info(const uint8_t* frame, size_t len,
                     JkCellInfo& cells, JkPackInfo& pack) {
    if (len < kJkResponseSize) return false;
    if (!starts_with_response_magic(frame, len)) return false;
    if (frame[4] != kJkFrameTypeCellInfo) return false;

    cells = {};
    pack  = {};

    auto looks_like_bitmask = [](uint32_t v) -> bool {
        // (1<<N)-1 for N>=4: lowest N bits set, none above.
        return v >= 0x0F && ((v + 1) & v) == 0;
    };
    const bool   is_32s      = !looks_like_bitmask(le32(frame + 54));
    const size_t cell_slots  = is_32s ? 32 : 24;
    const size_t cell_shift  = is_32s ? 16 : 0;
    const size_t telem_shift = is_32s ? 32 : 0;

    // ----- per-cell -----
    cells.voltages_mv.reserve(cell_slots);
    for (size_t i = 0; i < cell_slots; ++i) {
        const uint16_t v = le16(frame + 6 + i * 2);
        if (v == 0) break;
        cells.voltages_mv.push_back(v);
    }

    cells.average_voltage_mv  = le16(frame + 58 + cell_shift);
    cells.voltage_diff_mv     = le16(frame + 60 + cell_shift);
    // BMS reports max/min cell as 1-based; convert to 0-based.
    cells.max_voltage_cell_idx = frame[62 + cell_shift]
        ? static_cast<uint8_t>(frame[62 + cell_shift] - 1) : 0;
    cells.min_voltage_cell_idx = frame[63 + cell_shift]
        ? static_cast<uint8_t>(frame[63 + cell_shift] - 1) : 0;

    // Cell resistances follow the cell-voltage table: 24S → offset 64,
    // 32S → offset 80. One u16 LE per cell in µΩ (×0.001 mΩ on the wire).
    const size_t res_off = is_32s ? 80 : 64;
    cells.resistance_uohm.reserve(cells.voltages_mv.size());
    for (size_t i = 0; i < cells.voltages_mv.size(); ++i) {
        cells.resistance_uohm.push_back(le16(frame + res_off + i * 2));
    }

    // ----- pack-level -----
    // MOS-temp slot moved between revisions: 24S → byte 134; 32S → byte 144
    // (the latter falls inside the cell-resistance region of the older layout).
    pack.power_tube_temp_dC = is_32s
        ? le16s(frame + 144)
        : le16s(frame + 134);

    pack.voltage_mv     = static_cast<int32_t>(le32(frame + 118 + telem_shift));
    pack.current_ma     = le32s(frame + 126 + telem_shift);
    pack.battery_temp1_dC = le16s(frame + 130 + telem_shift);
    pack.battery_temp2_dC = le16s(frame + 132 + telem_shift);
    // Errors moved too: 24S byte 136, 32S byte 166.
    pack.errors_bitmask = le16(frame + (is_32s ? 166 : 136));

    pack.state_of_charge_pct    = frame[141 + telem_shift];
    pack.remaining_capacity_mah = le32(frame + 142 + telem_shift);
    pack.total_capacity_mah     = le32(frame + 146 + telem_shift);
    pack.cycle_count            = le32(frame + 150 + telem_shift);

    pack.charging_enabled    = frame[166 + telem_shift] != 0;
    pack.discharging_enabled = frame[167 + telem_shift] != 0;
    pack.balance_current_ma  = static_cast<int16_t>(le16(frame + 138 + telem_shift));
    pack.balancer_enabled    = frame[140 + telem_shift] != 0;

    return true;
}

bool parse_device_info(const uint8_t* frame, size_t len, JkDeviceInfo& out) {
    if (len < kJkResponseSize) return false;
    if (!starts_with_response_magic(frame, len)) return false;
    if (frame[4] != kJkFrameTypeDeviceInfo) return false;

    out = {};
    out.vendor_id          = read_string(frame +   6, 16);
    out.hardware_version   = read_string(frame +  22,  8);
    out.firmware_version   = read_string(frame +  30,  8);
    out.uptime_s           = le32(frame + 38);
    out.power_on_count     = le32(frame + 42);
    out.device_name        = read_string(frame +  46, 16);
    out.login_password     = read_string(frame +  62, 16);
    out.manufacturing_date = read_string(frame +  78,  8);
    out.serial_number      = read_string(frame +  86, 16);
    out.user_data_1        = read_string(frame + 102, 16);
    out.settings_password  = read_string(frame + 118, 16);
    out.user_data_2        = read_string(frame + 134, 16);
    out.uart1_protocol               = frame[184];
    out.can_protocol                 = frame[185];
    out.uart2_protocol               = frame[218];
    out.lcd_buzzer_trigger_protocol  = frame[234];
    out.lcd_buzzer_trigger_value     = le32(frame + 238);
    out.lcd_buzzer_release_value     = le32(frame + 242);
    out.data_store_period_s          = le32(frame + 262);
    out.rcv_time_01h                 = frame[266];
    out.rfv_time_01h                 = frame[267];
    out.emerg_time                   = frame[269];
    out.rebulk_soc_pct               = frame[278];
    return true;
}

// ---- parse_settings (frame_type 0x01) -------------------------------------
// Field layout follows esphome-jk-bms decode_jk02_settings_;
// Voltages and currents are u32 LE in mV / mA; temperatures are i32 LE in 0.1 °C.
bool parse_settings(const uint8_t* frame, size_t len, JkSettings& out) {
    if (len < kJkResponseSize) return false;
    if (!starts_with_response_magic(frame, len)) return false;
    if (frame[4] != kJkFrameTypeSettings) return false;

    out = {};
    out.smart_sleep_mv             = static_cast<uint16_t>(le32(frame +   6));
    out.cell_uvp_mv                = static_cast<uint16_t>(le32(frame +  10));
    out.cell_uvpr_mv               = static_cast<uint16_t>(le32(frame +  14));
    out.cell_ovp_mv                = static_cast<uint16_t>(le32(frame +  18));
    out.cell_ovpr_mv               = static_cast<uint16_t>(le32(frame +  22));
    out.balance_trigger_mv         = static_cast<uint16_t>(le32(frame +  26));
    out.soc_100_mv                 = static_cast<uint16_t>(le32(frame +  30));
    out.soc_0_mv                   = static_cast<uint16_t>(le32(frame +  34));
    out.request_charge_mv          = static_cast<uint16_t>(le32(frame +  38));
    out.request_float_mv           = static_cast<uint16_t>(le32(frame +  42));
    out.power_off_mv               = static_cast<uint16_t>(le32(frame +  46));
    out.max_charge_current_ma      = le32(frame +  50);
    out.charge_ocp_delay_s         = le32(frame +  54);
    out.charge_ocp_recovery_s      = le32(frame +  58);
    out.max_discharge_current_ma   = le32(frame +  62);
    out.discharge_ocp_delay_s      = le32(frame +  66);
    out.discharge_ocp_recovery_s   = le32(frame +  70);
    out.scp_recovery_s             = le32(frame +  74);
    out.max_balance_current_ma     = le32(frame +  78);
    out.charge_otp_dC              = static_cast<int16_t>(le32s(frame +  82));
    out.charge_otp_recovery_dC     = static_cast<int16_t>(le32s(frame +  86));
    out.discharge_otp_dC           = static_cast<int16_t>(le32s(frame +  90));
    out.discharge_otp_recovery_dC  = static_cast<int16_t>(le32s(frame +  94));
    out.charge_utp_dC              = static_cast<int16_t>(le32s(frame +  98));
    out.charge_utp_recovery_dC     = static_cast<int16_t>(le32s(frame + 102));
    out.mosfet_otp_dC              = static_cast<int16_t>(le32s(frame + 106));
    out.mosfet_otp_recovery_dC     = static_cast<int16_t>(le32s(frame + 110));
    out.cell_count                 = static_cast<uint8_t>(le32(frame + 114));
    out.charging_switch_on         = le32(frame + 118) != 0;
    out.discharging_switch_on      = le32(frame + 122) != 0;
    out.balancer_switch_on         = le32(frame + 126) != 0;
    out.nominal_capacity_mah       = le32(frame + 130);
    out.scp_delay_us               = le32(frame + 134);
    out.start_balance_mv           = static_cast<uint16_t>(le32(frame + 138));
    return true;
}

// ---- frame builders (JK02_32S layout, the offsets of the parsers) ----------

std::vector<uint8_t> build_cell_info_frame(const JkCellInfo& cells,
                                           const JkPackInfo& pack) {
    auto f = empty_frame(kJkFrameTypeCellInfo);

    const size_t n = std::min(cells.voltages_mv.size(), kMaxCells);
    for (size_t i = 0; i < n; ++i) {
        put16(f, 6 + i * 2, cells.voltages_mv[i]);
        if (i < cells.resistance_uohm.size())
            put16(f, 80 + i * 2, cells.resistance_uohm[i]);
    }
    put32(f, 70, n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1);   // enabled cells
    put16(f, 74, cells.average_voltage_mv);
    put16(f, 76, cells.voltage_diff_mv);
    if (n) {                                               // 1-based on the wire
        f[78] = static_cast<uint8_t>(cells.max_voltage_cell_idx + 1);
        f[79] = static_cast<uint8_t>(cells.min_voltage_cell_idx + 1);
    }

    put16(f, 144, static_cast<uint16_t>(pack.power_tube_temp_dC));
    put32(f, 150, static_cast<uint32_t>(pack.voltage_mv));
    const int64_t power_mw =
        static_cast<int64_t>(pack.voltage_mv) * pack.current_ma / 1000;
    put32(f, 154, static_cast<uint32_t>(power_mw < 0 ? -power_mw : power_mw));
    put32(f, 158, static_cast<uint32_t>(pack.current_ma));
    put16(f, 162, static_cast<uint16_t>(pack.battery_temp1_dC));
    put16(f, 164, static_cast<uint16_t>(pack.battery_temp2_dC));
    put16(f, 166, pack.errors_bitmask);
    put16(f, 170, static_cast<uint16_t>(pack.balance_current_ma));
    // Balancer action: 0 off, 1 charging the cell, 2 discharging it — the
    // direction is the sign of the balance current.
    f[172] = !pack.balancer_enabled ? 0 : pack.balance_current_ma < 0 ? 2 : 1;
    f[173] = pack.state_of_charge_pct;
    put32(f, 174, pack.remaining_capacity_mah);
    put32(f, 178, pack.total_capacity_mah);
    put32(f, 182, pack.cycle_count);
    f[190] = 100;                                          // state of health, %
    f[198] = pack.charging_enabled ? 1 : 0;
    f[199] = pack.discharging_enabled ? 1 : 0;

    seal_frame(f);
    return f;
}

std::vector<uint8_t> build_settings_frame(const JkSettings& s) {
    auto f = empty_frame(kJkFrameTypeSettings);
    auto temp = [&](size_t off, int16_t dC) {
        put32(f, off, static_cast<uint32_t>(static_cast<int32_t>(dC)));
    };

    put32(f,   6, s.smart_sleep_mv);
    put32(f,  10, s.cell_uvp_mv);
    put32(f,  14, s.cell_uvpr_mv);
    put32(f,  18, s.cell_ovp_mv);
    put32(f,  22, s.cell_ovpr_mv);
    put32(f,  26, s.balance_trigger_mv);
    put32(f,  30, s.soc_100_mv);
    put32(f,  34, s.soc_0_mv);
    put32(f,  38, s.request_charge_mv);
    put32(f,  42, s.request_float_mv);
    put32(f,  46, s.power_off_mv);
    put32(f,  50, s.max_charge_current_ma);
    put32(f,  54, s.charge_ocp_delay_s);
    put32(f,  58, s.charge_ocp_recovery_s);
    put32(f,  62, s.max_discharge_current_ma);
    put32(f,  66, s.discharge_ocp_delay_s);
    put32(f,  70, s.discharge_ocp_recovery_s);
    put32(f,  74, s.scp_recovery_s);
    put32(f,  78, s.max_balance_current_ma);
    temp( 82, s.charge_otp_dC);
    temp( 86, s.charge_otp_recovery_dC);
    temp( 90, s.discharge_otp_dC);
    temp( 94, s.discharge_otp_recovery_dC);
    temp( 98, s.charge_utp_dC);
    temp(102, s.charge_utp_recovery_dC);
    temp(106, s.mosfet_otp_dC);
    temp(110, s.mosfet_otp_recovery_dC);
    put32(f, 114, s.cell_count);
    put32(f, 118, s.charging_switch_on ? 1 : 0);
    put32(f, 122, s.discharging_switch_on ? 1 : 0);
    put32(f, 126, s.balancer_switch_on ? 1 : 0);
    put32(f, 130, s.nominal_capacity_mah);
    put32(f, 134, s.scp_delay_us);
    put32(f, 138, s.start_balance_mv);

    seal_frame(f);
    return f;
}

// ---- parallel RS485 link ---------------------------------------------------

uint16_t modbus_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                            : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

int try_parse_jk_rs485_request(const uint8_t* buf, size_t len,
                                  JkRs485Request& out) {
    // Fixed bytes of a poll; the address, the register low byte and the two
    // data bytes vary.
    auto mismatch = [&](size_t pos, uint8_t want) {
        return len > pos && buf[pos] != want;
    };
    if (mismatch(1, 0x10) || mismatch(2, 0x16) || mismatch(4, 0x00) ||
        mismatch(5, 0x01) || mismatch(6, 0x02))
        return -1;
    if (len < kJkRs485RequestSize) return 0;

    const uint16_t crc = modbus_crc16(buf, kJkRs485RequestSize - 2);
    if (buf[9] != (crc & 0xFF) || buf[10] != (crc >> 8)) return -1;

    out.address = buf[0];
    out.reg     = static_cast<uint16_t>(buf[2] << 8 | buf[3]);
    return static_cast<int>(kJkRs485RequestSize);
}

std::optional<uint8_t> jk_rs485_frame_type(uint16_t reg) {
    switch (reg) {
        case kJkRs485RegDeviceInfo: return kJkFrameTypeDeviceInfo;
        case kJkRs485RegSettings:   return kJkFrameTypeSettings;
        case kJkRs485RegCellInfo:   return kJkFrameTypeCellInfo;
        default:                       return std::nullopt;
    }
}

std::vector<uint8_t> build_jk_rs485_response(uint8_t address, uint16_t reg,
                                                const std::vector<uint8_t>& frame) {
    if (frame.size() != kJkResponseSize) return {};

    std::vector<uint8_t> out = frame;
    // Over Bluetooth byte 5 is a frame counter; on this link the master's own
    // frames carry its address there.
    out[5] = address;
    if (out[4] == kJkFrameTypeSettings)
        out[kSettingsDeviceAddressOffset] = address;
    out[kJkResponseSize - 1] = jk_crc8(out.data(), kJkResponseSize - 1);

    const uint8_t ack[6] = {address, 0x10,
                            static_cast<uint8_t>(reg >> 8),
                            static_cast<uint8_t>(reg & 0xFF), 0x00, 0x01};
    const uint16_t crc = modbus_crc16(ack, sizeof ack);
    out.insert(out.end(), ack, ack + sizeof ack);
    out.push_back(static_cast<uint8_t>(crc & 0xFF));
    out.push_back(static_cast<uint8_t>(crc >> 8));
    return out;
}
