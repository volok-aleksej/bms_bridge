#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Wire-format constants ------------------------------------------------------

constexpr size_t  kJkRequestSize       = 20;
constexpr size_t  kJkResponseSize      = 300;

// Magic headers
constexpr uint8_t kJkRequestMagic[4]   = {0xAA, 0x55, 0x90, 0xEB};
constexpr uint8_t kJkResponseMagic[4]  = {0x55, 0xAA, 0xEB, 0x90};

// Command codes accepted at byte 4 of a request.
constexpr uint8_t kJkCmdDeviceInfo     = 0x97;
constexpr uint8_t kJkCmdCellInfo       = 0x96;

// Frame type byte (offset 4) of a 300-byte response.
constexpr uint8_t kJkFrameTypeSettings   = 0x01;
constexpr uint8_t kJkFrameTypeCellInfo   = 0x02;
constexpr uint8_t kJkFrameTypeDeviceInfo = 0x03;

// Request (host → BMS) ------------------------------------------------------
//
// Fixed 20-byte layout:
//   [magic 4B] [cmd 1B] [value_length 1B] [value 4B LE] [padding 9B zero] [crc 1B]
//
// `value_length` is the number of meaningful bytes in `value` (0..4); the rest
// of the 13-byte data slot is always zero — JK firmware does not validate the
// padding (proven by esphome which sends zeros).
struct JkRequest {
    uint8_t  command = 0;
    uint8_t  value_length = 0;
    uint32_t value = 0;

    void serialize(std::vector<uint8_t>& out) const;
};

// Telemetry (BMS → host) ----------------------------------------------------

// frame_type = 0x03
struct JkDeviceInfo {
    std::string vendor_id;            // e.g. "JK_BD6A20S10P"
    std::string hardware_version;     // e.g. "10.XW"
    std::string firmware_version;     // e.g. "10.07"
    uint32_t    uptime_s = 0;
    uint32_t    power_on_count = 0;
    std::string device_name;
    std::string manufacturing_date;
    std::string serial_number;
};

//frame_type = 0x02
struct JkCellInfo {
    std::vector<uint16_t> voltages_mv;          // length = enabled cells (≤32)
    uint16_t average_voltage_mv = 0;
    uint16_t voltage_diff_mv = 0;
    uint8_t  max_voltage_cell_idx = 0;          // 0-based
    uint8_t  min_voltage_cell_idx = 0;          // 0-based
};

// frame_type = 0x01. Values come straight off
// the wire in JK-native units: mV, mA, 0.1 °C.
struct JkSettings {
    uint8_t  cell_count = 0;
    uint16_t cell_ovp_mv = 0;          // cell over-voltage protection
    uint16_t cell_uvp_mv = 0;          // cell under-voltage protection
    uint16_t cell_uvpr_mv = 0;         // cell UVP recovery (used as "low warning")
    int16_t  charge_otp_dC = 0;        // charge over-temperature
    int16_t  discharge_otp_dC = 0;     // discharge over-temperature
    int16_t  charge_utp_dC = 0;        // charge under-temperature (also reused for discharge low)
    uint32_t max_charge_current_ma = 0;
    uint32_t max_discharge_current_ma = 0;
};

// frame_type = 0x02
struct JkPackInfo {
    int32_t  voltage_mv = 0;
    int32_t  current_ma = 0;                    // sign: + charge, − discharge
    uint8_t  state_of_charge_pct = 0;
    uint32_t remaining_capacity_mah = 0;
    uint32_t total_capacity_mah = 0;
    uint32_t cycle_count = 0;

    int16_t  battery_temp1_dC = 0;              // 0.1 °C
    int16_t  battery_temp2_dC = 0;
    int16_t  power_tube_temp_dC = 0;            // MOS temperature

    bool charging_enabled = false;              // charge MOSFET on
    bool discharging_enabled = false;           // discharge MOSFET on
    bool balancer_enabled = false;

    uint16_t errors_bitmask = 0;
};

// Notify reassembler --------------------------------------------------------
//
// BLE notify chunks (~20 bytes each on default MTU) come in arbitrary slices
// and are interleaved with idle "AT\r\n" noise emitted by the JK BLE chip.
// This class:
//   - drops anything until it sees the response magic;
//   - accumulates 300 bytes of frame payload from there;
//   - validates the trailing CRC;
//   - hands the frame to the caller via take_complete_frame().
class JkFrameAssembler {
public:
    void push_chunk(const uint8_t* data, size_t len);
    std::optional<std::vector<uint8_t>> take_complete_frame();
    void reset();

private:
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> ready_;
};

// Free helpers --------------------------------------------------------------

uint8_t jk_crc8(const uint8_t* data, size_t len);
std::optional<uint8_t> jk_response_frame_type(const uint8_t* frame, size_t len);
bool parse_cell_info(const uint8_t* frame, size_t len,
                     JkCellInfo& cells_out, JkPackInfo& pack_out);
bool parse_device_info(const uint8_t* frame, size_t len, JkDeviceInfo& out);
bool parse_settings(const uint8_t* frame, size_t len, JkSettings& out);
