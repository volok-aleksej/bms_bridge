#include "history.hpp"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace {

constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS batteries (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS samples (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    battery_id          INTEGER NOT NULL REFERENCES batteries(id),
    ts_ms               INTEGER NOT NULL,
    voltage_mv          INTEGER NOT NULL,
    current_ma          INTEGER NOT NULL,
    soc_pct             INTEGER NOT NULL,
    remaining_mah       INTEGER NOT NULL,
    total_mah           INTEGER NOT NULL,
    cycle_count         INTEGER NOT NULL,
    temp1_dc            INTEGER NOT NULL,
    temp2_dc            INTEGER NOT NULL,
    mos_temp_dc         INTEGER NOT NULL,
    flags               INTEGER NOT NULL,
    errors_bitmask      INTEGER NOT NULL,
    avg_cell_mv         INTEGER NOT NULL,
    diff_cell_mv        INTEGER NOT NULL,
    max_cell_idx        INTEGER NOT NULL,
    min_cell_idx        INTEGER NOT NULL,
    balance_current_ma  INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_samples_bat_ts ON samples(battery_id, ts_ms);

CREATE TABLE IF NOT EXISTS daily_energy (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    battery_id    INTEGER NOT NULL REFERENCES batteries(id),
    date          TEXT    NOT NULL,
    charged_wh    REAL    NOT NULL DEFAULT 0,
    discharged_wh REAL    NOT NULL DEFAULT 0,
    UNIQUE(battery_id, date)
);
CREATE INDEX IF NOT EXISTS idx_daily_bat_date ON daily_energy(battery_id, date);

CREATE TABLE IF NOT EXISTS sample_cells (
    sample_id       INTEGER NOT NULL,
    cell_idx        INTEGER NOT NULL,
    voltage_mv      INTEGER NOT NULL,
    resistance_uohm INTEGER NOT NULL,
    PRIMARY KEY (sample_id, cell_idx),
    FOREIGN KEY (sample_id) REFERENCES samples(id) ON DELETE CASCADE
) WITHOUT ROWID;
)sql";

constexpr const char* kInsertSampleSql = R"sql(
INSERT INTO samples (
    battery_id, ts_ms, voltage_mv, current_ma, soc_pct, remaining_mah,
    total_mah, cycle_count, temp1_dc, temp2_dc, mos_temp_dc, flags,
    errors_bitmask, avg_cell_mv, diff_cell_mv, max_cell_idx, min_cell_idx,
    balance_current_ma
) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql";

constexpr const char* kInsertCellSql = R"sql(
INSERT INTO sample_cells (sample_id, cell_idx, voltage_mv, resistance_uohm)
VALUES (?,?,?,?)
)sql";

uint32_t pack_flags(const JkPackInfo& p) {
    uint32_t f = 0;
    if (p.charging_enabled)    f |= 0x1u;
    if (p.discharging_enabled) f |= 0x2u;
    if (p.balancer_enabled)    f |= 0x4u;
    return f;
}

}  // namespace

History::History(std::string db_path, std::chrono::seconds ram_window)
    : db_path_(std::move(db_path)),
      ram_window_(ram_window) {
    open_db();
    prepare_statements();
}

History::~History() {
    if (insert_sample_stmt_) sqlite3_finalize(insert_sample_stmt_);
    if (insert_cell_stmt_)   sqlite3_finalize(insert_cell_stmt_);
    if (rdb_)                sqlite3_close(rdb_);
    if (db_)                 sqlite3_close(db_);
}

void History::open_db() {
    int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("history: cannot open " + db_path_ + ": " + msg);
    }

    // WAL + NORMAL synchronous keeps writes well under 1 ms; concurrent
    // readers (HTTP server) don't block the writer.
    char* err = nullptr;
    auto exec = [&](const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error(std::string("history: ") + sql + ": " + m);
        }
    };
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous  = NORMAL");
    exec("PRAGMA temp_store   = MEMORY");
    exec("PRAGMA foreign_keys = ON");
    sqlite3_busy_timeout(db_, 5000);
    exec(kSchemaSql);


    load_battery_cache();

    if (sqlite3_open_v2(db_path_.c_str(), &rdb_,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        spdlog::warn("history: read-only connection open failed: {}",
                     rdb_ ? sqlite3_errmsg(rdb_) : "?");
        if (rdb_) { sqlite3_close(rdb_); rdb_ = nullptr; }
    } else {
        sqlite3_busy_timeout(rdb_, 2000);
    }

    spdlog::info("history: opened {}", db_path_);
}

void History::prepare_statements() {
    auto prep = [&](const char* sql, sqlite3_stmt** out) {
        if (sqlite3_prepare_v2(db_, sql, -1, out, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("history: prepare: ")
                                     + sqlite3_errmsg(db_));
        }
    };
    prep(kInsertSampleSql, &insert_sample_stmt_);
    prep(kInsertCellSql,   &insert_cell_stmt_);
}

int History::battery_row_id(sqlite3* conn, const std::string& name,
                            bool create) const {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = battery_id_cache_.find(name);
        if (it != battery_id_cache_.end()) return it->second;
    }
    if (!conn) return -1;

    int id = -1;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(conn, "SELECT id FROM batteries WHERE name = ?",
                           -1, &sel, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sel, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW) id = sqlite3_column_int(sel, 0);
        sqlite3_finalize(sel);
    }

    if (id < 0 && create) {
        sqlite3_stmt* ins = nullptr;
        if (sqlite3_prepare_v2(conn, "INSERT INTO batteries (name) VALUES (?)",
                               -1, &ins, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ins, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(ins) == SQLITE_DONE)
                id = static_cast<int>(sqlite3_last_insert_rowid(conn));
            else
                spdlog::warn("history: insert battery '{}': {}", name,
                             sqlite3_errmsg(conn));
            sqlite3_finalize(ins);
        }
    }

    if (id >= 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        battery_id_cache_[name] = id;
    }
    return id;
}

void History::append(const std::string& battery_id,
                     std::chrono::system_clock::time_point ts,
                     const JkCellInfo& cells, const JkPackInfo& pack) {
    const auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          ts.time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        ram_.push_back(TelemetrySample{battery_id, ts, cells, pack});
        const auto cutoff = std::chrono::system_clock::now() - ram_window_;
        while (!ram_.empty() && ram_.front().ts < cutoff) {
            ram_.pop_front();
        }
    }

    if (!insert_sample_stmt_ || !insert_cell_stmt_) return;

    const int bat_id = battery_row_id(db_, battery_id, /*create=*/true);
    if (bat_id < 0) {
        spdlog::warn("history: cannot resolve battery '{}', sample dropped",
                     battery_id);
        return;
    }

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        spdlog::warn("history: BEGIN failed: {}", sqlite3_errmsg(db_));
        return;
    }

    auto rollback = [&] {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };

    sqlite3_reset(insert_sample_stmt_);
    sqlite3_clear_bindings(insert_sample_stmt_);
    int i = 1;
    sqlite3_bind_int  (insert_sample_stmt_, i++, bat_id);
    sqlite3_bind_int64(insert_sample_stmt_, i++, static_cast<sqlite3_int64>(ts_ms));
    sqlite3_bind_int64(insert_sample_stmt_, i++, pack.voltage_mv);
    sqlite3_bind_int64(insert_sample_stmt_, i++, pack.current_ma);
    sqlite3_bind_int  (insert_sample_stmt_, i++, pack.state_of_charge_pct);
    sqlite3_bind_int64(insert_sample_stmt_, i++, pack.remaining_capacity_mah);
    sqlite3_bind_int64(insert_sample_stmt_, i++, pack.total_capacity_mah);
    sqlite3_bind_int64(insert_sample_stmt_, i++, pack.cycle_count);
    sqlite3_bind_int  (insert_sample_stmt_, i++, pack.battery_temp1_dC);
    sqlite3_bind_int  (insert_sample_stmt_, i++, pack.battery_temp2_dC);
    sqlite3_bind_int  (insert_sample_stmt_, i++, pack.power_tube_temp_dC);
    sqlite3_bind_int  (insert_sample_stmt_, i++, static_cast<int>(pack_flags(pack)));
    sqlite3_bind_int  (insert_sample_stmt_, i++, pack.errors_bitmask);
    sqlite3_bind_int  (insert_sample_stmt_, i++, cells.average_voltage_mv);
    sqlite3_bind_int  (insert_sample_stmt_, i++, cells.voltage_diff_mv);
    sqlite3_bind_int  (insert_sample_stmt_, i++, cells.max_voltage_cell_idx);
    sqlite3_bind_int  (insert_sample_stmt_, i++, cells.min_voltage_cell_idx);
    sqlite3_bind_int  (insert_sample_stmt_, i++, pack.balance_current_ma);

    if (sqlite3_step(insert_sample_stmt_) != SQLITE_DONE) {
        spdlog::warn("history: insert sample failed: {}", sqlite3_errmsg(db_));
        rollback();
        return;
    }

    const sqlite3_int64 sample_id = sqlite3_last_insert_rowid(db_);
    const size_t n_cells = cells.voltages_mv.size();

    for (size_t k = 0; k < n_cells; ++k) {
        const uint16_t r_uohm = (k < cells.resistance_uohm.size())
                                    ? cells.resistance_uohm[k] : 0;

        sqlite3_reset(insert_cell_stmt_);
        sqlite3_clear_bindings(insert_cell_stmt_);
        sqlite3_bind_int64(insert_cell_stmt_, 1, sample_id);
        sqlite3_bind_int  (insert_cell_stmt_, 2, static_cast<int>(k));
        sqlite3_bind_int  (insert_cell_stmt_, 3, cells.voltages_mv[k]);
        sqlite3_bind_int  (insert_cell_stmt_, 4, r_uohm);

        if (sqlite3_step(insert_cell_stmt_) != SQLITE_DONE) {
            spdlog::warn("history: insert cell {} failed: {}", k,
                         sqlite3_errmsg(db_));
            rollback();
            return;
        }
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        spdlog::warn("history: COMMIT failed: {}", sqlite3_errmsg(db_));
        rollback();
    }
}

std::vector<TelemetrySample> History::recent(const std::string& battery_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<TelemetrySample> out;
    for (const auto& s : ram_)
        if (s.battery_id == battery_id) out.push_back(s);
    return out;
}

int History::ensure_battery_id(const std::string& name) {
    return battery_row_id(db_, name, /*create=*/true);
}

void History::load_battery_cache() {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, name FROM batteries",
                           -1, &st, nullptr) != SQLITE_OK) {
        spdlog::warn("history: prepare load_battery_cache: {}",
                     sqlite3_errmsg(db_));
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const int id = sqlite3_column_int(st, 0);
        const unsigned char* v = sqlite3_column_text(st, 1);
        battery_id_cache_[v ? reinterpret_cast<const char*>(v) : ""] = id;
    }
    sqlite3_finalize(st);
}

std::vector<History::BatteryRef> History::battery_list() const {
    std::vector<BatteryRef> out;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        out.reserve(battery_id_cache_.size());
        for (const auto& [name, id] : battery_id_cache_)
            out.push_back({id, name});
    }
    std::sort(out.begin(), out.end(),
              [](const BatteryRef& a, const BatteryRef& b) {
                  return a.id < b.id;
              });
    return out;
}

std::string History::battery_name(int battery_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& [name, id] : battery_id_cache_)
        if (id == battery_id) return name;
    return {};
}

std::vector<TelemetrySample> History::range(
        const std::string& battery_id,
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to,
        int limit, int64_t step_ms) const {
    if (!rdb_) return {};
    const int bat_id = battery_row_id(rdb_, battery_id, /*create=*/false);
    if (bat_id < 0) return {};  // unknown battery → no rows
    return range_core(bat_id, battery_id, from, to, limit, step_ms);
}

std::vector<TelemetrySample> History::range_by_id(
        int battery_id,
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to,
        int limit, int64_t step_ms) const {
    if (!rdb_ || battery_id <= 0) return {};
    return range_core(battery_id, battery_name(battery_id),
                      from, to, limit, step_ms);
}

std::vector<TelemetrySample> History::range_core(
        int bat_id, const std::string& name_for_result,
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to,
        int limit, int64_t step_ms) const {
    std::vector<TelemetrySample> out;
    if (!rdb_) return out;

    const auto from_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            from.time_since_epoch()).count();
    const auto to_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                            to.time_since_epoch()).count();

    // Plain query (no decimation)
    constexpr const char* kSelectSamplesSql = R"sql(
        SELECT id, ts_ms, voltage_mv, current_ma, soc_pct,
               remaining_mah, total_mah, cycle_count,
               temp1_dc, temp2_dc, mos_temp_dc,
               flags, errors_bitmask,
               avg_cell_mv, diff_cell_mv, max_cell_idx, min_cell_idx,
               balance_current_ma
        FROM samples
        WHERE battery_id = ? AND ts_ms >= ? AND ts_ms < ?
        ORDER BY ts_ms DESC
        LIMIT ?
    )sql";

    // Decimated query: newest record per time bucket of step_ms size.
    // A window function does this in a single bounded index pass
    constexpr const char* kSelectDecimatedSql = R"sql(
        SELECT id, ts_ms, voltage_mv, current_ma, soc_pct,
               remaining_mah, total_mah, cycle_count,
               temp1_dc, temp2_dc, mos_temp_dc,
               flags, errors_bitmask,
               avg_cell_mv, diff_cell_mv, max_cell_idx, min_cell_idx,
               balance_current_ma
        FROM (
            SELECT *, ROW_NUMBER() OVER (
                          PARTITION BY (ts_ms - ?) / ?
                          ORDER BY ts_ms DESC) AS rn
            FROM samples
            WHERE battery_id = ? AND ts_ms >= ? AND ts_ms < ?
        )
        WHERE rn = 1
        ORDER BY ts_ms DESC
        LIMIT ?
    )sql";
    constexpr const char* kSelectCellsSql = R"sql(
        SELECT cell_idx, voltage_mv, resistance_uohm
        FROM sample_cells
        WHERE sample_id = ?
        ORDER BY cell_idx ASC
    )sql";

    sqlite3_stmt* sst = nullptr;
    sqlite3_stmt* cst = nullptr;
    const char* sql = (step_ms > 0) ? kSelectDecimatedSql : kSelectSamplesSql;
    if (sqlite3_prepare_v2(rdb_, sql, -1, &sst, nullptr) != SQLITE_OK) {
        spdlog::warn("history: prepare range/samples: {}", sqlite3_errmsg(rdb_));
        return out;
    }
    if (sqlite3_prepare_v2(rdb_, kSelectCellsSql, -1, &cst, nullptr) != SQLITE_OK) {
        spdlog::warn("history: prepare range/cells: {}", sqlite3_errmsg(rdb_));
        sqlite3_finalize(sst);
        return out;
    }

    if (step_ms > 0) {
        sqlite3_bind_int64(sst, 1, from_ms);   // PARTITION BY (ts_ms - ?)
        sqlite3_bind_int64(sst, 2, step_ms);   //              / ?
        sqlite3_bind_int  (sst, 3, bat_id);    // WHERE battery_id = ?
        sqlite3_bind_int64(sst, 4, from_ms);   //   AND ts_ms >= ?
        sqlite3_bind_int64(sst, 5, to_ms);     //   AND ts_ms <  ?
        sqlite3_bind_int  (sst, 6, limit > 0 ? limit : -1);
    } else {
        sqlite3_bind_int  (sst, 1, bat_id);
        sqlite3_bind_int64(sst, 2, from_ms);
        sqlite3_bind_int64(sst, 3, to_ms);
        sqlite3_bind_int  (sst, 4, limit > 0 ? limit : -1);
    }

    while (sqlite3_step(sst) == SQLITE_ROW) {
        TelemetrySample s;
        s.battery_id = name_for_result;
        const sqlite3_int64 sample_id = sqlite3_column_int64(sst, 0);
        const sqlite3_int64 ts_ms = sqlite3_column_int64(sst, 1);
        s.ts = std::chrono::system_clock::time_point(
                   std::chrono::milliseconds(ts_ms));

        s.pack.voltage_mv             = static_cast<int32_t>(sqlite3_column_int64(sst, 2));
        s.pack.current_ma             = static_cast<int32_t>(sqlite3_column_int64(sst, 3));
        s.pack.state_of_charge_pct    = static_cast<uint8_t>(sqlite3_column_int(sst, 4));
        s.pack.remaining_capacity_mah = static_cast<uint32_t>(sqlite3_column_int64(sst, 5));
        s.pack.total_capacity_mah     = static_cast<uint32_t>(sqlite3_column_int64(sst, 6));
        s.pack.cycle_count            = static_cast<uint32_t>(sqlite3_column_int64(sst, 7));
        s.pack.battery_temp1_dC       = static_cast<int16_t>(sqlite3_column_int(sst, 8));
        s.pack.battery_temp2_dC       = static_cast<int16_t>(sqlite3_column_int(sst, 9));
        s.pack.power_tube_temp_dC     = static_cast<int16_t>(sqlite3_column_int(sst, 10));
        const uint32_t flags          = static_cast<uint32_t>(sqlite3_column_int(sst, 11));
        s.pack.charging_enabled       = (flags & 0x1u) != 0;
        s.pack.discharging_enabled    = (flags & 0x2u) != 0;
        s.pack.balancer_enabled       = (flags & 0x4u) != 0;
        s.pack.errors_bitmask         = static_cast<uint16_t>(sqlite3_column_int(sst, 12));
        s.pack.balance_current_ma     = static_cast<int16_t>(sqlite3_column_int(sst, 17));
        s.cells.average_voltage_mv    = static_cast<uint16_t>(sqlite3_column_int(sst, 13));
        s.cells.voltage_diff_mv       = static_cast<uint16_t>(sqlite3_column_int(sst, 14));
        s.cells.max_voltage_cell_idx  = static_cast<uint8_t>(sqlite3_column_int(sst, 15));
        s.cells.min_voltage_cell_idx  = static_cast<uint8_t>(sqlite3_column_int(sst, 16));

        sqlite3_reset(cst);
        sqlite3_clear_bindings(cst);
        sqlite3_bind_int64(cst, 1, sample_id);
        while (sqlite3_step(cst) == SQLITE_ROW) {
            s.cells.voltages_mv.push_back(
                static_cast<uint16_t>(sqlite3_column_int(cst, 1)));
            s.cells.resistance_uohm.push_back(
                static_cast<uint16_t>(sqlite3_column_int(cst, 2)));
        }

        out.push_back(std::move(s));
    }

    sqlite3_finalize(cst);
    sqlite3_finalize(sst);
    return out;
}

void History::aggregate_pending() {
    time_t now_t = std::chrono::system_clock::to_time_t(
                       std::chrono::system_clock::now());
    struct tm gmt{};
    gmtime_r(&now_t, &gmt);
    gmt.tm_hour = gmt.tm_min = gmt.tm_sec = 0;
    gmt.tm_isdst = 0;
    const int64_t today_ms = (int64_t)timegm(&gmt) * 1000;
    // Raw samples older than 8 days are purged; daily_energy keeps everything.
    const int64_t purge_ms = today_ms - 8LL * 86400000LL;

    constexpr const char* kFindPending = R"sql(
        SELECT DISTINCT s.battery_id, date(s.ts_ms/1000, 'unixepoch') AS d
        FROM samples s
        LEFT JOIN daily_energy de
               ON de.battery_id = s.battery_id
              AND de.date = date(s.ts_ms/1000, 'unixepoch')
        WHERE s.ts_ms < ? AND de.id IS NULL
        ORDER BY s.battery_id, d
    )sql";

    sqlite3_stmt* find_st = nullptr;
    if (sqlite3_prepare_v2(db_, kFindPending, -1, &find_st, nullptr) != SQLITE_OK) {
        spdlog::warn("history: aggregate_pending prepare: {}", sqlite3_errmsg(db_));
        return;
    }
    sqlite3_bind_int64(find_st, 1, today_ms);

    std::vector<std::pair<int, std::string>> pending;
    while (sqlite3_step(find_st) == SQLITE_ROW) {
        int bat = sqlite3_column_int(find_st, 0);
        const auto* d = sqlite3_column_text(find_st, 1);
        if (d) pending.push_back({bat, reinterpret_cast<const char*>(d)});
    }
    sqlite3_finalize(find_st);

    if (pending.empty()) return;
    spdlog::info("history: aggregating {} day(s)", pending.size());

    constexpr const char* kGetSamples = R"sql(
        SELECT ts_ms, voltage_mv, current_ma
        FROM samples
        WHERE battery_id = ? AND ts_ms >= ? AND ts_ms < ?
        ORDER BY ts_ms ASC
    )sql";
    constexpr const char* kInsertDaily = R"sql(
        INSERT OR REPLACE INTO daily_energy(battery_id, date, charged_wh, discharged_wh)
        VALUES(?,?,?,?)
    )sql";

    for (const auto& [bat_id, date_s] : pending) {
        struct tm t{};
        strptime(date_s.c_str(), "%Y-%m-%d", &t);
        t.tm_isdst = 0;
        const int64_t day_start = (int64_t)timegm(&t) * 1000;
        const int64_t day_end   = day_start + 86400000LL;

        // Trapezoidal energy integration over the day's raw samples
        double charged = 0.0, discharged = 0.0;
        {
            sqlite3_stmt* sst = nullptr;
            if (sqlite3_prepare_v2(db_, kGetSamples, -1, &sst, nullptr) != SQLITE_OK) {
                spdlog::warn("history: aggregate load samples: {}", sqlite3_errmsg(db_));
                continue;
            }
            sqlite3_bind_int  (sst, 1, bat_id);
            sqlite3_bind_int64(sst, 2, day_start);
            sqlite3_bind_int64(sst, 3, day_end);

            int64_t prev_ts  = 0;
            double  prev_pw  = 0.0;
            bool    has_prev = false;
            while (sqlite3_step(sst) == SQLITE_ROW) {
                const int64_t ts = sqlite3_column_int64(sst, 0);
                // P [W] = voltage_mV/1000 * current_mA/1000
                const double pw = (double)sqlite3_column_int(sst, 1)
                                * (double)sqlite3_column_int(sst, 2)
                                / 1.0e6;
                if (has_prev) {
                    const double dt_h = (double)(ts - prev_ts) / 3600000.0;
                    const double dwh  = (prev_pw + pw) / 2.0 * dt_h;
                    if (dwh > 0.0) charged    += dwh;
                    else           discharged -= dwh;
                }
                prev_ts = ts; prev_pw = pw; has_prev = true;
            }
            sqlite3_finalize(sst);
        }

        // Write to daily_energy and delete raw samples atomically
        if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
            spdlog::warn("history: aggregate BEGIN: {}", sqlite3_errmsg(db_));
            continue;
        }

        bool ok = false;
        sqlite3_stmt* ins = nullptr;
        if (sqlite3_prepare_v2(db_, kInsertDaily, -1, &ins, nullptr) == SQLITE_OK) {
            sqlite3_bind_int   (ins, 1, bat_id);
            sqlite3_bind_text  (ins, 2, date_s.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(ins, 3, charged);
            sqlite3_bind_double(ins, 4, discharged);
            ok = (sqlite3_step(ins) == SQLITE_DONE);
        }
        if (ins) sqlite3_finalize(ins);

        if (ok && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) {
            spdlog::info("history: aggregated bat={} {} ch={:.3f}Wh dis={:.3f}Wh",
                         bat_id, date_s, charged, discharged);
        } else {
            spdlog::warn("history: aggregate failed bat={} {}: {}",
                         bat_id, date_s, sqlite3_errmsg(db_));
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    // Purge raw samples older than 8 days regardless of aggregation status
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM samples WHERE ts_ms < ?",
                           -1, &del, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(del, 1, purge_ms);
        if (sqlite3_step(del) == SQLITE_DONE) {
            const int purged = sqlite3_changes(db_);
            if (purged > 0)
                spdlog::info("history: purged {} raw sample(s) older than 8 days", purged);
        }
        sqlite3_finalize(del);
    }
}

std::vector<DailyEnergy> History::get_daily_energy(
        int battery_id, int year, int month) const {
    if (!rdb_) return {};

    char month_start[11], month_end[11];
    snprintf(month_start, sizeof(month_start), "%04d-%02d-01", year, month);
    int ny = year, nm = month + 1;
    if (nm > 12) { nm = 1; ++ny; }
    snprintf(month_end, sizeof(month_end), "%04d-%02d-01", ny, nm);

    constexpr const char* kSql = R"sql(
        SELECT battery_id, date, charged_wh, discharged_wh
        FROM daily_energy
        WHERE battery_id = ? AND date >= ? AND date < ?
        ORDER BY date ASC
    )sql";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rdb_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        spdlog::warn("history: get_daily_energy: {}", sqlite3_errmsg(rdb_));
        return {};
    }
    sqlite3_bind_int (st, 1, battery_id);
    sqlite3_bind_text(st, 2, month_start, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, month_end,   -1, SQLITE_TRANSIENT);

    std::vector<DailyEnergy> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        DailyEnergy e;
        e.battery_id    = sqlite3_column_int(st, 0);
        const auto* d   = sqlite3_column_text(st, 1);
        e.date          = d ? reinterpret_cast<const char*>(d) : "";
        e.charged_wh    = sqlite3_column_double(st, 2);
        e.discharged_wh = sqlite3_column_double(st, 3);
        e.battery_name  = battery_name(e.battery_id);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}
