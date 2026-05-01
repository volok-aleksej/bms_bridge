#pragma once

#include <string>

struct AppConfig {
    std::string device_uuid;
    int log_level = 4;
    std::string log_file;

    std::string service_uuid;
    std::string char_write_uuid;
    std::string char_notify_uuid;
    int scan_timeout_ms = 10000;
    int adapter_poll_ms = 2000;
    int reconnect_backoff_ms = 5000;
    int bms_poll_period_ms = 5000;
    int settings_refresh_period_ms = 60000;

    std::string uart_device;
    int uart_baud = 115200;
    int pylontech_address = 2;
};

bool load_config(const std::string& path, AppConfig& out, std::string& error);
