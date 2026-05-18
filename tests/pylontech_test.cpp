#include "pylontech.hpp"
#include "shared_state.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

// Real frames captured from the Deye inverter polling our emulator over UART.
constexpr std::array<uint8_t, 20> kSniffedAnalogRequest = {
    0x7E, '2', '0', '0', '2', '4', '6', '4', '2',
    'E', '0', '0', '2',  '0', '2',
    'F', 'D', '3', '3', 0x0D,
};

// LENGTH says LENID=2 (one info byte) but INFO is empty — known Deye quirk.
constexpr std::array<uint8_t, 18> kSniffedSystemParamRequest = {
    0x7E, '2', '0', '0', '2', '4', '6', '4', '7',
    'E', '0', '0', '2',
    'F', 'D', '9', '0', 0x0D,
};

// ---- try_parse_pylontech --------------------------------------------------

TEST(TryParsePylontech, ParsesCapturedAnalogRequest) {
    PylontechFrame f;
    int n = try_parse_pylontech(kSniffedAnalogRequest.data(),
                                kSniffedAnalogRequest.size(), f);
    EXPECT_EQ(n, static_cast<int>(kSniffedAnalogRequest.size()));
    EXPECT_EQ(f.ver,  0x20);
    EXPECT_EQ(f.adr,  0x02);
    EXPECT_EQ(f.cid1, 0x46);
    EXPECT_EQ(f.cid2, 0x42);
    ASSERT_EQ(f.info.size(), 1u);
    EXPECT_EQ(f.info[0], 0x02);
}

TEST(TryParsePylontech, ToleratesEmptyInfoEvenWhenLenidNonZero) {
    PylontechFrame f;
    int n = try_parse_pylontech(kSniffedSystemParamRequest.data(),
                                kSniffedSystemParamRequest.size(), f);
    EXPECT_EQ(n, static_cast<int>(kSniffedSystemParamRequest.size()));
    EXPECT_EQ(f.cid2, 0x47);
    EXPECT_TRUE(f.info.empty());
}

TEST(TryParsePylontech, ReturnsZeroOnEmptyBuffer) {
    PylontechFrame f;
    EXPECT_EQ(try_parse_pylontech(nullptr, 0, f), 0);
}

TEST(TryParsePylontech, ReturnsZeroWhenNoEoiYet) {
    auto truncated = kSniffedAnalogRequest;
    PylontechFrame f;
    EXPECT_EQ(try_parse_pylontech(truncated.data(), truncated.size() - 1, f), 0);
}

TEST(TryParsePylontech, DropsJunkBeforeSoi) {
    std::vector<uint8_t> noisy{0xAA, 0xBB, 0xCC};
    noisy.insert(noisy.end(), kSniffedAnalogRequest.begin(),
                              kSniffedAnalogRequest.end());
    PylontechFrame f;
    int n = try_parse_pylontech(noisy.data(), noisy.size(), f);
    EXPECT_EQ(n, -3);  // discard 3 junk bytes; caller retries from offset 3
}

TEST(TryParsePylontech, RejectsCorruptedChecksum) {
    auto bad = kSniffedAnalogRequest;
    bad[16] = '0';  // flip a CHKSUM digit
    PylontechFrame f;
    int n = try_parse_pylontech(bad.data(), bad.size(), f);
    EXPECT_LT(n, 0);
}

// ---- build → parse round-trip --------------------------------------------

namespace {

PylontechFrame make_request(uint8_t cid2, uint8_t addr_byte = 0x02) {
    PylontechFrame req;
    req.ver  = 0x20;
    req.adr  = 0x02;
    req.cid1 = 0x46;
    req.cid2 = cid2;
    req.info = {addr_byte};
    return req;
}

PylontechFrame parse_or_fail(const std::vector<uint8_t>& wire) {
    PylontechFrame f;
    int n = try_parse_pylontech(wire.data(), wire.size(), f);
    EXPECT_EQ(n, static_cast<int>(wire.size()));
    return f;
}

JkCellInfo sample_cells() {
    JkCellInfo c;
    c.voltages_mv = std::vector<uint16_t>(16, 3290);
    return c;
}

JkPackInfo sample_pack() {
    JkPackInfo p;
    p.voltage_mv             = 52'671;
    p.current_ma             = -23'751;
    p.battery_temp1_dC       = 220;
    p.battery_temp2_dC       = 209;
    p.power_tube_temp_dC     = 230;
    p.state_of_charge_pct    = 74;
    p.remaining_capacity_mah = 233'329;
    p.total_capacity_mah     = 314'000;
    p.cycle_count            = 30;
    p.charging_enabled       = true;
    p.discharging_enabled    = true;
    return p;
}

JkSettings sample_settings() {
    JkSettings s;
    s.cell_count               = 16;
    s.cell_ovp_mv              = 3650;
    s.cell_uvp_mv              = 2500;
    s.cell_uvpr_mv             = 3000;
    s.charge_otp_dC            = 500;
    s.charge_utp_dC            = -100;
    s.discharge_otp_dC         = 600;
    s.max_charge_current_ma    = 100'000;
    s.max_discharge_current_ma = 200'000;
    return s;
}

}  // namespace

TEST(BuildPylontech, FrameRoundTripsThroughParser) {
    PylontechFrame in;
    in.ver = 0x20; in.adr = 0x02; in.cid1 = 0x46; in.cid2 = 0x42;
    in.info = {0x11, 0x22, 0x33};

    auto out = parse_or_fail(build_pylontech(in));
    EXPECT_EQ(out.ver,  in.ver);
    EXPECT_EQ(out.adr,  in.adr);
    EXPECT_EQ(out.cid1, in.cid1);
    EXPECT_EQ(out.cid2, in.cid2);
    EXPECT_EQ(out.info, in.info);
}

// ---- Builders -------------------------------------------------------------

TEST(BuildAnalogResponse, ParsesCleanly) {
    auto cells = sample_cells();
    auto pack  = sample_pack();
    auto resp = parse_or_fail(build_analog_response(make_request(0x42), cells, pack));
    EXPECT_EQ(resp.cid2, 0x00);   // RTN normal
}

TEST(BuildAlarmResponse, ParsesCleanly) {
    auto cells = sample_cells();
    auto resp = parse_or_fail(build_alarm_response(make_request(0x44), cells));
    EXPECT_EQ(resp.cid2, 0x00);
}

TEST(BuildSystemParamResponse, ParsesCleanly) {
    auto s = sample_settings();
    auto resp = parse_or_fail(build_system_param_response(make_request(0x47), s));
    EXPECT_EQ(resp.cid2, 0x00);
}

TEST(BuildChgmgmtResponse, ParsesCleanly) {
    auto pack = sample_pack();
    auto s    = sample_settings();
    auto resp = parse_or_fail(build_chgmgmt_response(make_request(0x92), pack, s));
    EXPECT_EQ(resp.cid2, 0x00);
}

// ---- Master-side request builders -----------------------------------------

TEST(BuildAnalogRequest, RoundTripsThroughParser) {
    auto f = parse_or_fail(build_analog_request(0x03));
    EXPECT_EQ(f.ver,  0x20);
    EXPECT_EQ(f.adr,  0x03);
    EXPECT_EQ(f.cid1, 0x46);
    EXPECT_EQ(f.cid2, 0x42);
    ASSERT_EQ(f.info.size(), 1u);
    EXPECT_EQ(f.info[0], 0x03);     // DATAINFO = COMMAND = adr
}

TEST(BuildSystemParamRequest, RoundTripsThroughParser) {
    auto f = parse_or_fail(build_system_param_request(0x05));
    EXPECT_EQ(f.adr,  0x05);
    EXPECT_EQ(f.cid1, 0x46);
    EXPECT_EQ(f.cid2, 0x47);
    EXPECT_TRUE(f.info.empty());
}

// ---- Master-side deserialize (inverse of serialize) -----------------------

TEST(AnalogResponse, SerializeDeserializeRoundTrip) {
    AnalogResponse in;
    in.infoflag            = 0x11;
    in.command             = 0x02;
    in.cell_voltages_mv    = {3290, 3301, 3312, 3283};
    in.temperatures_k10    = {2982, 2981, 2983};
    in.current_01a         = -237;
    in.module_voltage_mv   = 52'671;
    in.remain_capacity_mah = 23'329;
    in.user_defined_count  = 2;
    in.total_capacity_mah  = 31'400;
    in.cycle_count         = 30;

    std::vector<uint8_t> info;
    in.serialize(info);

    AnalogResponse out;
    ASSERT_TRUE(out.deserialize(info));
    EXPECT_EQ(out.infoflag,            in.infoflag);
    EXPECT_EQ(out.command,             in.command);
    EXPECT_EQ(out.cell_voltages_mv,    in.cell_voltages_mv);
    EXPECT_EQ(out.temperatures_k10,    in.temperatures_k10);
    EXPECT_EQ(out.current_01a,         in.current_01a);
    EXPECT_EQ(out.module_voltage_mv,   in.module_voltage_mv);
    EXPECT_EQ(out.remain_capacity_mah, in.remain_capacity_mah);
    EXPECT_EQ(out.user_defined_count,  in.user_defined_count);
    EXPECT_EQ(out.total_capacity_mah,  in.total_capacity_mah);
    EXPECT_EQ(out.cycle_count,         in.cycle_count);
}

TEST(AnalogResponse, DeserializeRejectsShortBuffer) {
    std::vector<uint8_t> info = {0x00, 0x02, 0x04, 0x0C, 0xDA, 0x0C};
    AnalogResponse out;
    EXPECT_FALSE(out.deserialize(info));
}

TEST(SystemParameterResponse, SerializeDeserializeRoundTrip) {
    SystemParameterResponse in;
    in.infoflag                      = 0x00;
    in.cell_high_voltage_limit_mv    = 3650;
    in.cell_low_voltage_limit_mv     = 3000;
    in.cell_under_voltage_limit_mv   = 2500;
    in.charge_high_temp_k10          = 3231;
    in.charge_low_temp_k10           = 2621;
    in.charge_current_limit_01a      = 1000;
    in.module_high_voltage_limit_mv  = 58'400;
    in.module_low_voltage_limit_mv   = 48'000;
    in.module_under_voltage_limit_mv = 40'000;
    in.discharge_high_temp_k10       = 3331;
    in.discharge_low_temp_k10        = 2521;
    in.discharge_current_limit_01a   = -2000;

    std::vector<uint8_t> info;
    in.serialize(info);

    SystemParameterResponse out;
    ASSERT_TRUE(out.deserialize(info));
    EXPECT_EQ(out.infoflag,                      in.infoflag);
    EXPECT_EQ(out.cell_high_voltage_limit_mv,    in.cell_high_voltage_limit_mv);
    EXPECT_EQ(out.cell_low_voltage_limit_mv,     in.cell_low_voltage_limit_mv);
    EXPECT_EQ(out.cell_under_voltage_limit_mv,   in.cell_under_voltage_limit_mv);
    EXPECT_EQ(out.charge_high_temp_k10,          in.charge_high_temp_k10);
    EXPECT_EQ(out.charge_low_temp_k10,           in.charge_low_temp_k10);
    EXPECT_EQ(out.charge_current_limit_01a,      in.charge_current_limit_01a);
    EXPECT_EQ(out.module_high_voltage_limit_mv,  in.module_high_voltage_limit_mv);
    EXPECT_EQ(out.module_low_voltage_limit_mv,   in.module_low_voltage_limit_mv);
    EXPECT_EQ(out.module_under_voltage_limit_mv, in.module_under_voltage_limit_mv);
    EXPECT_EQ(out.discharge_high_temp_k10,       in.discharge_high_temp_k10);
    EXPECT_EQ(out.discharge_low_temp_k10,        in.discharge_low_temp_k10);
    EXPECT_EQ(out.discharge_current_limit_01a,   in.discharge_current_limit_01a);
}

TEST(SystemParameterResponse, DeserializeRejectsShortBuffer) {
    std::vector<uint8_t> info = {0x00, 0x0E, 0x42};  // far too short
    SystemParameterResponse out;
    EXPECT_FALSE(out.deserialize(info));
}


TEST(AnalogResponse, RoundTripsThroughSlaveBuilder) {
    JkCellInfo cells;
    cells.voltages_mv = {3290, 3305, 3298, 3301};
    JkPackInfo pack;
    pack.voltage_mv             = 52'671;
    pack.current_ma             = -23'700;   // → -237 in 0.1 A
    pack.battery_temp1_dC       = 220;
    pack.battery_temp2_dC       = 209;
    pack.power_tube_temp_dC     = 230;
    pack.remaining_capacity_mah = 37'000;
    pack.total_capacity_mah     = 50'000;    // ≤ 65000 → not scaled
    pack.cycle_count            = 30;

    auto f = parse_or_fail(build_analog_response(make_request(0x42), cells, pack));
    AnalogResponse a;
    ASSERT_TRUE(a.deserialize(f.info));
    EXPECT_EQ(a.cell_voltages_mv,    cells.voltages_mv);
    EXPECT_EQ(a.current_01a,         -237);
    EXPECT_EQ(a.module_voltage_mv,   52'671);
    EXPECT_EQ(a.remain_capacity_mah, 37'000);
    EXPECT_EQ(a.total_capacity_mah,  50'000);
    EXPECT_EQ(a.cycle_count,         30);
    ASSERT_EQ(a.temperatures_k10.size(), 3u);
}
