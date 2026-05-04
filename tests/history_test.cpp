#include "history.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

JkCellInfo make_cells(uint8_t n = 4) {
    JkCellInfo c;
    c.voltages_mv.assign(n, 3300);
    c.resistance_uohm.assign(n, 200);
    c.average_voltage_mv = 3300;
    c.voltage_diff_mv    = 5;
    c.max_voltage_cell_idx = 1;
    c.min_voltage_cell_idx = 2;
    return c;
}

JkPackInfo make_pack() {
    JkPackInfo p;
    p.voltage_mv             = 13200;
    p.current_ma             = -1500;
    p.state_of_charge_pct    = 87;
    p.remaining_capacity_mah = 87000;
    p.total_capacity_mah     = 100000;
    p.cycle_count            = 42;
    p.battery_temp1_dC       = 250;
    p.battery_temp2_dC       = 251;
    p.power_tube_temp_dC     = 305;
    p.charging_enabled       = true;
    p.discharging_enabled    = true;
    p.balancer_enabled       = false;
    p.errors_bitmask         = 0;
    return p;
}

std::string make_temp_db_path(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
             / ("bms_history_test_" + std::string(tag) + "_"
                + std::to_string(::getpid()) + ".sqlite3");
    std::filesystem::remove(p);
    // SQLite WAL also leaves -wal/-shm sidecars; remove them too.
    std::filesystem::remove(std::filesystem::path(p.string() + "-wal"));
    std::filesystem::remove(std::filesystem::path(p.string() + "-shm"));
    return p.string();
}

}  // namespace

TEST(History, RangeWidensToSeeMoreSamples) {
    using namespace std::chrono;

    const std::string path = make_temp_db_path("range");
    {
        // RAM window irrelevant for this test — range() reads the DB.
        History h(path, hours(48));

        const auto today        = system_clock::now();
        const auto yesterday    = today - hours(24);
        const auto two_days_ago = today - hours(48);

        const auto cells = make_cells();
        const auto pack  = make_pack();

        h.append(two_days_ago, cells, pack);
        h.append(yesterday,    cells, pack);
        h.append(today,        cells, pack);

        // Window covers only "today" → 1 record.
        EXPECT_EQ(h.range(today - hours(1), today + hours(1)).size(), 1u);

        // Window covers yesterday and today → 2 records.
        EXPECT_EQ(h.range(today - hours(25), today + hours(1)).size(), 2u);

        // Window covers all three → 3 records.
        EXPECT_EQ(h.range(today - hours(72), today + hours(1)).size(), 3u);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(std::filesystem::path(path + "-wal"));
    std::filesystem::remove(std::filesystem::path(path + "-shm"));
}

TEST(History, RangePreservesCellArrays) {
    using namespace std::chrono;

    const std::string path = make_temp_db_path("cells");
    {
        History h(path, hours(48));

        const auto t = system_clock::now();
        JkCellInfo c;
        c.voltages_mv     = {3301, 3302, 3303, 3304};
        c.resistance_uohm = {201, 202, 203, 204};
        c.average_voltage_mv   = 3302;
        c.voltage_diff_mv      = 3;
        c.max_voltage_cell_idx = 3;
        c.min_voltage_cell_idx = 0;

        h.append(t, c, make_pack());

        const auto rows = h.range(t - hours(1), t + hours(1));
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].cells.voltages_mv,     c.voltages_mv);
        EXPECT_EQ(rows[0].cells.resistance_uohm, c.resistance_uohm);
        EXPECT_EQ(rows[0].cells.average_voltage_mv,   c.average_voltage_mv);
        EXPECT_EQ(rows[0].cells.voltage_diff_mv,      c.voltage_diff_mv);
        EXPECT_EQ(rows[0].cells.max_voltage_cell_idx, c.max_voltage_cell_idx);
        EXPECT_EQ(rows[0].cells.min_voltage_cell_idx, c.min_voltage_cell_idx);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(std::filesystem::path(path + "-wal"));
    std::filesystem::remove(std::filesystem::path(path + "-shm"));
}
