#include "history.hpp"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cstring>
#include <stdexcept>

namespace {

constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS samples (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms           INTEGER NOT NULL,
    voltage_mv      INTEGER NOT NULL,
    current_ma      INTEGER NOT NULL,
    soc_pct         INTEGER NOT NULL,
    remaining_mah   INTEGER NOT NULL,
    total_mah       INTEGER NOT NULL,
    cycle_count     INTEGER NOT NULL,
    temp1_dc        INTEGER NOT NULL,
    temp2_dc        INTEGER NOT NULL,
    mos_temp_dc     INTEGER NOT NULL,
    flags           INTEGER NOT NULL,
    errors_bitmask  INTEGER NOT NULL,
    avg_cell_mv     INTEGER NOT NULL,
    diff_cell_mv    INTEGER NOT NULL,
    max_cell_idx    INTEGER NOT NULL,
    min_cell_idx    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples(ts_ms);

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
    ts_ms, voltage_mv, current_ma, soc_pct, remaining_mah, total_mah,
    cycle_count, temp1_dc, temp2_dc, mos_temp_dc, flags, errors_bitmask,
    avg_cell_mv, diff_cell_mv, max_cell_idx, min_cell_idx
) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
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
    exec(kSchemaSql);

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

void History::append(std::chrono::system_clock::time_point ts,
                     const JkCellInfo& cells, const JkPackInfo& pack) {
    const auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          ts.time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        ram_.push_back(TelemetrySample{ts, cells, pack});
        const auto cutoff = std::chrono::system_clock::now() - ram_window_;
        while (!ram_.empty() && ram_.front().ts < cutoff) {
            ram_.pop_front();
        }
    }

    if (!insert_sample_stmt_ || !insert_cell_stmt_) return;

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

std::vector<TelemetrySample> History::recent() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return std::vector<TelemetrySample>(ram_.begin(), ram_.end());
}

std::vector<TelemetrySample> History::range(
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to,
        int limit, int64_t step_ms) const {
    std::vector<TelemetrySample> out;
    if (!db_) return out;

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
               avg_cell_mv, diff_cell_mv, max_cell_idx, min_cell_idx
        FROM samples
        WHERE ts_ms >= ? AND ts_ms < ?
        ORDER BY ts_ms DESC
        LIMIT ?
    )sql";

    // Decimated query: one record (newest) per time bucket of step_ms size
    constexpr const char* kSelectDecimatedSql = R"sql(
        SELECT s.id, s.ts_ms, s.voltage_mv, s.current_ma, s.soc_pct,
               s.remaining_mah, s.total_mah, s.cycle_count,
               s.temp1_dc, s.temp2_dc, s.mos_temp_dc,
               s.flags, s.errors_bitmask,
               s.avg_cell_mv, s.diff_cell_mv, s.max_cell_idx, s.min_cell_idx
        FROM samples s
        INNER JOIN (
            SELECT MAX(ts_ms) AS ts_ms
            FROM samples
            WHERE ts_ms >= ? AND ts_ms < ?
            GROUP BY (ts_ms - ?) / ?
        ) b ON s.ts_ms = b.ts_ms
        ORDER BY s.ts_ms DESC
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
    if (sqlite3_prepare_v2(db_, sql, -1, &sst, nullptr) != SQLITE_OK) {
        spdlog::warn("history: prepare range/samples: {}", sqlite3_errmsg(db_));
        return out;
    }
    if (sqlite3_prepare_v2(db_, kSelectCellsSql, -1, &cst, nullptr) != SQLITE_OK) {
        spdlog::warn("history: prepare range/cells: {}", sqlite3_errmsg(db_));
        sqlite3_finalize(sst);
        return out;
    }

    if (step_ms > 0) {
        sqlite3_bind_int64(sst, 1, static_cast<sqlite3_int64>(from_ms));
        sqlite3_bind_int64(sst, 2, static_cast<sqlite3_int64>(to_ms));
        sqlite3_bind_int64(sst, 3, static_cast<sqlite3_int64>(from_ms));
        sqlite3_bind_int64(sst, 4, static_cast<sqlite3_int64>(step_ms));
        sqlite3_bind_int  (sst, 5, limit > 0 ? limit : -1);
    } else {
        sqlite3_bind_int64(sst, 1, static_cast<sqlite3_int64>(from_ms));
        sqlite3_bind_int64(sst, 2, static_cast<sqlite3_int64>(to_ms));
        sqlite3_bind_int  (sst, 3, limit > 0 ? limit : -1);
    }

    while (sqlite3_step(sst) == SQLITE_ROW) {
        TelemetrySample s;
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
