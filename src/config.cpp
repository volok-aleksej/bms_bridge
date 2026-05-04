#include "config.hpp"

#include <confuse.h>

#include <cstring>

bool load_config(const std::string& path, AppConfig& out, std::string& error) {
    cfg_opt_t opts[] = {
        CFG_STR("device_uuid",         const_cast<char*>(""), CFGF_NONE),
        CFG_INT("log_level",           3, CFGF_NONE),
        CFG_STR("log_file",            const_cast<char*>(""), CFGF_NONE),
        CFG_STR("service_uuid",        nullptr, CFGF_NODEFAULT),
        CFG_STR("char_write_uuid",     nullptr, CFGF_NODEFAULT),
        CFG_STR("char_notify_uuid",    nullptr, CFGF_NODEFAULT),
        CFG_INT("scan_timeout_ms",     10000, CFGF_NONE),
        CFG_INT("adapter_poll_ms",     2000,  CFGF_NONE),
        CFG_INT("reconnect_backoff_ms",5000,  CFGF_NONE),
        CFG_INT("bms_poll_period_ms",  5000,  CFGF_NONE),
        CFG_INT("settings_refresh_period_ms", 60000, CFGF_NONE),
        CFG_STR("uart_device",         nullptr, CFGF_NODEFAULT),
        CFG_INT("uart_baud",           115200, CFGF_NONE),
        CFG_INT("pylontech_address",      2, CFGF_NONE),
        CFG_STR("history_db_path",
                const_cast<char*>("/var/lib/bms_bridge/history.sqlite3"),
                CFGF_NONE),
        CFG_INT("history_ram_window_s", 86400, CFGF_NONE),
        CFG_END()
    };

    cfg_t* cfg = cfg_init(opts, CFGF_NONE);
    int rc = cfg_parse(cfg, path.c_str());
    if (rc == CFG_FILE_ERROR) {
        error = "cannot open config file: " + path + " (" + std::strerror(errno) + ")";
        cfg_free(cfg);
        return false;
    }
    if (rc == CFG_PARSE_ERROR) {
        error = "parse error in config file: " + path;
        cfg_free(cfg);
        return false;
    }

    auto str_or_empty = [&](const char* name) -> std::string {
        const char* v = cfg_getstr(cfg, name);
        return v ? std::string(v) : std::string();
    };

    out.device_uuid          = str_or_empty("device_uuid");
    out.log_level            = static_cast<int>(cfg_getint(cfg, "log_level"));
    out.log_file             = str_or_empty("log_file");
    out.service_uuid         = str_or_empty("service_uuid");
    out.char_write_uuid      = str_or_empty("char_write_uuid");
    out.char_notify_uuid     = str_or_empty("char_notify_uuid");
    out.scan_timeout_ms      = static_cast<int>(cfg_getint(cfg, "scan_timeout_ms"));
    out.adapter_poll_ms      = static_cast<int>(cfg_getint(cfg, "adapter_poll_ms"));
    out.reconnect_backoff_ms = static_cast<int>(cfg_getint(cfg, "reconnect_backoff_ms"));
    out.bms_poll_period_ms   = static_cast<int>(cfg_getint(cfg, "bms_poll_period_ms"));
    out.settings_refresh_period_ms = static_cast<int>(cfg_getint(cfg, "settings_refresh_period_ms"));
    out.uart_device          = str_or_empty("uart_device");
    out.uart_baud            = static_cast<int>(cfg_getint(cfg, "uart_baud"));
    out.pylontech_address    = static_cast<int>(cfg_getint(cfg, "pylontech_address"));
    out.history_db_path      = str_or_empty("history_db_path");
    out.history_ram_window_s = static_cast<int>(cfg_getint(cfg, "history_ram_window_s"));

    cfg_free(cfg);

    auto require = [&](const std::string& v, const char* name) {
        if (v.empty()) {
            error = std::string("config: required field '") + name + "' is missing or empty";
            return false;
        }
        return true;
    };
    if (!require(out.service_uuid,     "service_uuid"))     return false;
    if (!require(out.char_write_uuid,  "char_write_uuid"))  return false;
    if (!require(out.char_notify_uuid, "char_notify_uuid")) return false;
    if (!require(out.uart_device,      "uart_device"))      return false;

    return true;
}
