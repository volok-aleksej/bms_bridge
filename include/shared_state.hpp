#pragma once

#include "jk_protocol.hpp"

#include <chrono>
#include <mutex>
#include <optional>

// Snapshot of everything the BLE side has observed about the BMS so far.
struct BmsSnapshot {
    bool connected = false;
    std::optional<JkCellInfo> cells;
    std::optional<JkPackInfo> pack;
    std::optional<JkSettings> settings;
    std::chrono::steady_clock::time_point updated_at{};
};

class SharedState {
public:
    void set_connected(bool v) {
        std::lock_guard<std::mutex> lk(mtx_);
        connected_ = v;
    }

    void apply_telemetry(JkCellInfo c, JkPackInfo p) {
        std::lock_guard<std::mutex> lk(mtx_);
        cells_      = std::move(c);
        pack_       = std::move(p);
        updated_at_ = std::chrono::steady_clock::now();
    }

    void apply_settings(JkSettings s) {
        std::lock_guard<std::mutex> lk(mtx_);
        settings_ = std::move(s);
    }

    BmsSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return BmsSnapshot{connected_, cells_, pack_, settings_, updated_at_};
    }

    bool is_fresh(std::chrono::milliseconds max_age) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!connected_ || !cells_ || !pack_) return false;
        return (std::chrono::steady_clock::now() - updated_at_) <= max_age;
    }

private:
    mutable std::mutex mtx_;
    bool connected_ = false;
    std::optional<JkCellInfo> cells_;
    std::optional<JkPackInfo> pack_;
    std::optional<JkSettings> settings_;
    std::chrono::steady_clock::time_point updated_at_{};
};
