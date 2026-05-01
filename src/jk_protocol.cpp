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
    pack.balancer_enabled    = frame[169 + telem_shift] != 0;

    return true;
}

bool parse_device_info(const uint8_t* frame, size_t len, JkDeviceInfo& out) {
    if (len < kJkResponseSize) return false;
    if (!starts_with_response_magic(frame, len)) return false;
    if (frame[4] != kJkFrameTypeDeviceInfo) return false;

    out = {};
    out.vendor_id          = read_string(frame +  6, 16);
    out.hardware_version   = read_string(frame + 22,  8);
    out.firmware_version   = read_string(frame + 30,  8);
    out.uptime_s           = le32(frame + 38);
    out.power_on_count     = le32(frame + 42);
    out.device_name        = read_string(frame + 46, 16);
    out.manufacturing_date = read_string(frame + 78,  8);
    out.serial_number      = read_string(frame + 86, 11);
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
    out.cell_uvp_mv             = static_cast<uint16_t>(le32(frame +  10));
    out.cell_uvpr_mv            = static_cast<uint16_t>(le32(frame +  14));
    out.cell_ovp_mv             = static_cast<uint16_t>(le32(frame +  18));
    out.max_charge_current_ma   = le32(frame +  50);
    out.max_discharge_current_ma= le32(frame +  62);
    out.charge_otp_dC           = static_cast<int16_t>(le32s(frame +  82));
    out.discharge_otp_dC        = static_cast<int16_t>(le32s(frame +  90));
    out.charge_utp_dC           = static_cast<int16_t>(le32s(frame +  98));
    out.cell_count              = static_cast<uint8_t>(le32(frame + 114));
    return true;
}
