#include "history.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

constexpr const char* kBat = "b1";

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

        h.append(kBat, two_days_ago, cells, pack);
        h.append(kBat, yesterday,    cells, pack);
        h.append(kBat, today,        cells, pack);

        // Window covers only "today" → 1 record.
        EXPECT_EQ(h.range(kBat, today - hours(1), today + hours(1)).size(), 1u);

        // Window covers yesterday and today → 2 records.
        EXPECT_EQ(h.range(kBat, today - hours(25), today + hours(1)).size(), 2u);

        // Window covers all three → 3 records.
        EXPECT_EQ(h.range(kBat, today - hours(72), today + hours(1)).size(), 3u);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(std::filesystem::path(path + "-wal"));
    std::filesystem::remove(std::filesystem::path(path + "-shm"));
}

// Simulate the server-side pagination loop (handle_history_initial +
// handle_history_next) directly against History::range(), without starting an
// HTTP server.
TEST(History, PaginationNoOverlap) {
    using namespace std::chrono;

    constexpr int kCount       = 150;    // records per page
    constexpr int kRecords     = kCount * 2; // 2× ensures second page exists
    constexpr int kIntervalSec = 300;

    const std::string path = make_temp_db_path("pagination");
    {
        History h(path, hours(48));

        const auto t_end   = system_clock::now();
        const auto t_start = t_end - seconds(static_cast<long>(kRecords) * kIntervalSec);

        for (int i = 0; i < kRecords; ++i)
            h.append(kBat, t_start + seconds(static_cast<long>(i) * kIntervalSec),
                     make_cells(), make_pack());

        // — initial page —
        const auto win_ms = duration_cast<milliseconds>(t_end - t_start).count();
        const int64_t step_ms = win_ms / kCount;
        auto data = h.range(kBat, t_start - seconds(1), t_end + seconds(1),
                            kCount, step_ms);

        ASSERT_EQ(static_cast<int>(data.size()), kCount)
            << "initial page should be full";

        int page = 1;
        std::vector<int64_t> all_ts;
        int64_t prev_oldest_ts = INT64_MAX;
        auto time_start = t_start - seconds(1);

        while (true) {
            // timestamps strictly descending within a page
            for (size_t i = 1; i < data.size(); ++i)
                EXPECT_LT(data[i].ts, data[i - 1].ts)
                    << "page " << page << ": not DESC at index " << i;

            // newest record of this page is strictly older than oldest of previous page
            if (page > 1) {
                EXPECT_LT(data.front().ts.time_since_epoch().count(),
                          prev_oldest_ts)
                    << "page " << page << ": overlap with previous page";
            }

            // no duplicates
            for (const auto& s : data) {
                const auto ts = s.ts.time_since_epoch().count();
                EXPECT_EQ(std::count(all_ts.begin(), all_ts.end(), ts), 0)
                    << "duplicate ts on page " << page;
                all_ts.push_back(ts);
            }

            prev_oldest_ts = data.back().ts.time_since_epoch().count();

            // probe: is there anything older than the oldest record on this page?
            auto probe = h.range(kBat, time_start, data.back().ts, 1, 0);
            if (probe.empty()) break; // last page

            // next page
            auto time_end_next = data.back().ts;
            const auto win_next = duration_cast<milliseconds>(
                                      time_end_next - time_start).count();
            const int64_t step_next = (win_next > 0 && kCount > 0) ? win_next / kCount : 0;
            data = h.range(kBat, time_start, time_end_next, kCount, step_next);
            ++page;
        }

        EXPECT_GT(page, 1) << "expected at least two pages";
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

        h.append(kBat, t, c, make_pack());

        const auto rows = h.range(kBat, t - hours(1), t + hours(1));
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
