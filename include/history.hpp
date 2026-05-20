#pragma once

#include "jk_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

struct DailyEnergy {
    int         battery_id    = 0;
    std::string battery_name;
    std::string date;           // "YYYY-MM-DD" UTC
    double      charged_wh    = 0.0;
    double      discharged_wh = 0.0;
};

struct TelemetrySample {
    std::string battery_id;
    std::chrono::system_clock::time_point ts;
    JkCellInfo cells;
    JkPackInfo pack;
};

class History {
public:
    History(std::string db_path, std::chrono::seconds ram_window);
    ~History();

    History(const History&) = delete;
    History& operator=(const History&) = delete;

    void append(const std::string& battery_id,
                std::chrono::system_clock::time_point ts,
                const JkCellInfo& cells, const JkPackInfo& pack);
    std::vector<TelemetrySample> recent(const std::string& battery_id) const;

    // Ensure a `batteries` row exists for `name` and return its id.
    // Reserve batteries never append at all, but still physically exist
    // and must appear in the list / be queryable).
    int ensure_battery_id(const std::string& name);

    // Every row of the `batteries` table: {row id, name}.
    // The cache is pre-filled from the DB at startup and
    // kept current by the lazy insert in append(),
    // The legacy pre-multi-battery row has name "".
    struct BatteryRef { int id; std::string name; };
    std::vector<BatteryRef> battery_list() const;


    std::string battery_name(int battery_id) const;

    std::vector<TelemetrySample> range(const std::string& battery_id,
                                       std::chrono::system_clock::time_point from,
                                       std::chrono::system_clock::time_point to,
                                       int     limit   = -1,
                                       int64_t step_ms = 0) const;
    std::vector<TelemetrySample> range_by_id(int battery_id,
                                       std::chrono::system_clock::time_point from,
                                       std::chrono::system_clock::time_point to,
                                       int     limit   = -1,
                                       int64_t step_ms = 0) const;

    // Aggregate all complete past UTC days not yet in daily_energy, then purge
    // the raw sample rows for those days.  Handles first-run migration too.
    void aggregate_pending();

    // All daily_energy rows for battery_id in the given calendar month (UTC).
    std::vector<DailyEnergy> get_daily_energy(int battery_id,
                                              int year, int month) const;

private:
    void open_db();
    void prepare_statements();

    // Resolve a battery name to its `batteries.id` using connection `conn`.
    // With create=true a missing row is inserted (append path, write conn);
    // with create=false a missing name yields -1 (read path, read conn).
    // Cached.
    int battery_row_id(sqlite3* conn, const std::string& name,
                       bool create) const;

    void load_battery_cache();

    // Shared body of range()/range_by_id()
    std::vector<TelemetrySample> range_core(
        int bat_id, const std::string& name_for_result,
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to,
        int limit, int64_t step_ms) const;

    std::string db_path_;
    std::chrono::seconds ram_window_;

    sqlite3*      db_                  = nullptr;
    sqlite3*      rdb_                 = nullptr;
    sqlite3_stmt* insert_sample_stmt_  = nullptr;
    sqlite3_stmt* insert_cell_stmt_    = nullptr;

    mutable std::mutex mtx_;
    std::deque<TelemetrySample> ram_;
    mutable std::unordered_map<std::string, int> battery_id_cache_;
};
