#pragma once

#include "jk_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

struct TelemetrySample {
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

    // Append one cell_info sample. Pushes to the in-memory ring and writes
    // to SQLite. SQLite write errors are logged but do not throw — the RAM
    // ring keeps working so the HTTP recent-window endpoint still responds.
    void append(std::chrono::system_clock::time_point ts,
                const JkCellInfo& cells, const JkPackInfo& pack);

    // Snapshot copy of the in-memory ring (newest last). Cheap-ish: copies
    // up to ~3 MB under a mutex; HTTP server is expected to call this from
    // its own thread.
    std::vector<TelemetrySample> recent() const;

    // Read samples from SQLite with ts in [from, to), sorted newest-first.
    // limit < 0 means no limit.
    // step_ms > 0: return one record per time bucket of that size (decimation).
    std::vector<TelemetrySample> range(std::chrono::system_clock::time_point from,
                                       std::chrono::system_clock::time_point to,
                                       int     limit   = -1,
                                       int64_t step_ms = 0) const;

private:
    void open_db();
    void prepare_statements();

    std::string db_path_;
    std::chrono::seconds ram_window_;

    sqlite3*      db_                  = nullptr;
    sqlite3_stmt* insert_sample_stmt_  = nullptr;
    sqlite3_stmt* insert_cell_stmt_    = nullptr;

    mutable std::mutex mtx_;
    std::deque<TelemetrySample> ram_;
};
