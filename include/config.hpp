#pragma once

#include <string>
#include <vector>

enum class BatteryLink {
    Bluetooth,
    Ttl,
    Reserve,
};

struct BatteryConfig {
    std::string name;
    BatteryLink link;
    std::string link_raw;
    std::string address;
    int         capacity;
    std::string protocol;

    // --- bluetooth-specific ---
    std::string service_uuid     = "0000ffe0-0000-1000-8000-00805f9b34fb";
    std::string char_write_uuid  = "0000ffe1-0000-1000-8000-00805f9b34fb";
    std::string char_notify_uuid = "0000ffe1-0000-1000-8000-00805f9b34fb";
    int scan_timeout_ms = 10000;
    int adapter_poll_ms = 2000;

    // --- ttl-specific ---
    int uart_baud = 115200;

    // --- pylontech-specific ---
    // Module address as set on the battery (1..16). The wire ADR is this
    // value + 1 (the Pylontech protocol numbers modules from 2).
    int pylontech_address = 0;

    // --- reserve-specific ---
    // Name of the monitored battery this reserve mirrors (voltage / cells /
    // temps come from it; required for reserve).
    std::string mirror;

    // --- common optional (monitored links) ---
    int reconnect_ms = 5000;
    int bms_poll_period_ms = 5000;
    int settings_refresh_period_ms = 60000;
    int bms_state_update_interval_ms = 500;

    bool monitored() const {
        return link == BatteryLink::Bluetooth || link == BatteryLink::Ttl;
    }
};

struct InverterConfig {
    std::string uart_device;
    int uart_baud = 115200;
};

struct HttpConfig {
    int http_port = 80;
    std::string www_root = "/usr/share/bms_bridge/www";
};

struct HistoryConfig {
    std::string db_path = "/var/lib/bms_bridge/history.sqlite3";
    int ram_window_s = 86400;
};

struct AppConfig {
    int log_level = 4;
    std::string log_file;

    std::vector<BatteryConfig> batteries;
    InverterConfig inverter;
    HttpConfig     http;
    HistoryConfig  history;
};

bool load_config(const std::string& path, AppConfig& out, std::string& error);
