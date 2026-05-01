#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct PylontechFrame {
    uint8_t ver  = 0x20;
    uint8_t adr  = 0;
    uint8_t cid1 = 0x46;
    uint8_t cid2 = 0;
    std::vector<uint8_t> info;
};

// CID2 = 0x42 — analog data
struct AnalogResponse {
    uint8_t  infoflag = 0;
    uint8_t  command  = 0;
    std::vector<uint16_t> cell_voltages_mv;
    std::vector<uint16_t> temperatures_k10;
    int16_t  current_01a = 0;
    uint16_t module_voltage_mv = 0;
    uint16_t remain_capacity_mah = 0;
    uint8_t  user_defined_count = 2;
    uint16_t total_capacity_mah = 0;
    uint16_t cycle_count = 0;

    void serialize(std::vector<uint8_t>& info) const;
};

// CID2 = 0x44 — alarm / status flags.
struct AlarmResponse {
    uint8_t dataflag = 0;
    uint8_t command  = 0;
    std::vector<uint8_t> cell_voltage_states;
    std::vector<uint8_t> temperature_states;
    uint8_t charge_current_state = 0;
    uint8_t module_voltage_state = 0;
    uint8_t discharge_current_state = 0;
    uint8_t status_byte_1 = 0;
    uint8_t status_byte_2 = 0;
    uint8_t status_byte_3 = 0;
    uint8_t cell_error_states_1_8 = 0;
    uint8_t cell_error_states_9_16 = 0;

    void serialize(std::vector<uint8_t>& info) const;
};

// CID2 = 0x47 — system limits
struct SystemParameterResponse {
    uint8_t  infoflag = 0;
    uint16_t cell_high_voltage_limit_mv = 0;
    uint16_t cell_low_voltage_limit_mv = 0;
    uint16_t cell_under_voltage_limit_mv = 0;
    uint16_t charge_high_temp_k10 = 0;
    uint16_t charge_low_temp_k10 = 0;
    int16_t  charge_current_limit_01a = 0;
    uint16_t module_high_voltage_limit_mv = 0;
    uint16_t module_low_voltage_limit_mv = 0;
    uint16_t module_under_voltage_limit_mv = 0;
    uint16_t discharge_high_temp_k10 = 0;
    uint16_t discharge_low_temp_k10 = 0;
    int16_t  discharge_current_limit_01a = 0;

    void serialize(std::vector<uint8_t>& info) const;
};

// CID2 = 0x92 — charge/discharge management directives + battery permissions.
struct ChargeManagementResponse {
    uint8_t  command = 0;
    uint16_t charge_voltage_limit_mv = 0;
    uint16_t discharge_voltage_limit_mv = 0;
    int16_t  charge_current_limit_01a = 0;       // positive: max charge current
    int16_t  discharge_current_limit_01a = 0;    // negative: max discharge current
    bool charge_enabled = false;
    bool discharge_enabled = false;
    bool request_charge_immediately = false;
    bool request_charge_immediately_low = false;
    bool request_full_charge = false;

    void serialize(std::vector<uint8_t>& info) const;
};

struct JkCellInfo;
struct JkPackInfo;
struct JkSettings;

uint8_t module_count_for_capacity(uint32_t total_capacity_mah);

// Try to consume a single frame from the head of `buf`.
//   > 0 — number of bytes consumed; `out` populated.
//   = 0 — need more bytes (no complete frame yet).
//   < 0 — magnitude is the number of bytes that should be discarded
//         (junk before SOI, or unrecoverable malformed frame).
int try_parse_pylontech(const uint8_t* buf, size_t len, PylontechFrame& out);

std::vector<uint8_t> build_pylontech(const PylontechFrame& f);

std::vector<uint8_t> build_analog_response(const PylontechFrame& req,
                                           const JkCellInfo& cells,
                                           const JkPackInfo& pack);
std::vector<uint8_t> build_alarm_response(const PylontechFrame& req,
                                          const JkCellInfo& cells);
std::vector<uint8_t> build_system_param_response(const PylontechFrame& req,
                                                 const JkSettings& limits);
std::vector<uint8_t> build_chgmgmt_response(const PylontechFrame& req,
                                            const JkPackInfo& pack,
                                            const JkSettings& limits);
