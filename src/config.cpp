#include "config.hpp"

#include <confuse.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace {

bool link_from_str(const std::string& s, BatteryLink& out) {
    if (s == "bluetooth")    out = BatteryLink::Bluetooth;
    else if (s == "ttl")     out = BatteryLink::Ttl;
    else if (s == "reserve") out = BatteryLink::Reserve;
    else return false;
    return true;
}

bool is_supported_protocol(const std::string& p) {
    return p == "jk_bt" || p == "pylontech";
}

int vf_type(cfg_t* cfg, cfg_opt_t* opt) {
    const char* v = cfg_opt_getnstr(opt, 0);
    BatteryLink l;
    if (!v || !link_from_str(v, l)) {
        cfg_error(cfg, "battery '%s': type must be bluetooth|ttl|reserve",
                  cfg_title(cfg));
        return -1;
    }
    return 0;
}

int vf_protocol(cfg_t* cfg, cfg_opt_t* opt) {
    const char* v = cfg_opt_getnstr(opt, 0);
    std::string p = v ? v : "";
    if (!p.empty() && !is_supported_protocol(p)) {
        cfg_error(cfg, "battery '%s': unsupported protocol '%s'",
                  cfg_title(cfg), p.c_str());
        return -1;
    }
    return 0;
}

int vf_battery(cfg_t* cfg, cfg_opt_t* opt) {
    cfg_t* sec = cfg_opt_getnsec(opt, cfg_opt_size(opt) - 1);
    if (!sec) return 0;

    const char* proto = cfg_getstr(sec, "protocol");
    if (proto && std::string(proto) == "pylontech") {
        long addr = cfg_getint(sec, "pylontech_address");
        if (addr < 1 || addr > 16) {
            cfg_error(cfg, "battery '%s': 'pylontech_address' must be 1..16",
                      cfg_title(sec));
            return -1;
        }
    }

    const char* type = cfg_getstr(sec, "type");
    if (type && std::string(type) == "reserve") {
        const char* mir = cfg_getstr(sec, "mirror");
        if (!mir || mir[0] == '\0') {
            cfg_error(cfg, "reserve battery '%s': 'mirror' is required",
                      cfg_title(sec));
            return -1;
        }
    }
    return 0;
}

cfg_opt_t battery_opts[] = {
    // required for every battery
    CFG_STR("type",     nullptr, CFGF_NODEFAULT),
    CFG_STR("address",  nullptr, CFGF_NODEFAULT),
    CFG_INT("capacity", 0,       CFGF_NODEFAULT),
    CFG_STR("protocol", nullptr, CFGF_NODEFAULT),

    // bluetooth link (jk_bt)
    CFG_STR("service_uuid",     "0000ffe0-0000-1000-8000-00805f9b34fb", CFGF_NONE),
    CFG_STR("char_write_uuid",  "0000ffe1-0000-1000-8000-00805f9b34fb", CFGF_NONE),
    CFG_STR("char_notify_uuid", "0000ffe1-0000-1000-8000-00805f9b34fb", CFGF_NONE),
    CFG_INT("scan_timeout_ms",  10000, CFGF_NONE),
    CFG_INT("adapter_poll_ms",  2000,  CFGF_NONE),

    // ttl link / pylontech protocol
    CFG_INT("pylontech_address", 0, CFGF_NODEFAULT),
    CFG_INT("uart_baud", 115200, CFGF_NONE),

    // reserve link
    CFG_STR("mirror", nullptr, CFGF_NODEFAULT),

    // common to monitored links
    CFG_INT("reconnect_ms",                 5000,  CFGF_NONE),
    CFG_INT("bms_poll_period_ms",           5000,  CFGF_NONE),
    CFG_INT("settings_refresh_period_ms",   60000, CFGF_NONE),
    CFG_INT("bms_state_update_interval_ms", 500,   CFGF_NONE),
    CFG_END()
};

cfg_opt_t inverter_opts[] = {
    CFG_STR("uart", nullptr, CFGF_NODEFAULT),
    CFG_INT("baud", 115200,  CFGF_NONE),
    CFG_END()
};

cfg_opt_t http_opts[] = {
    CFG_INT("port", 80, CFGF_NONE),
    CFG_STR("www_root", "/usr/share/bms_bridge/www", CFGF_NONE),
    CFG_END()
};

cfg_opt_t history_opts[] = {
    CFG_STR("db_path", "/var/lib/bms_bridge/history.sqlite3", CFGF_NONE),
    CFG_INT("ram_window_s", 86400, CFGF_NONE),
    CFG_END()
};

std::string sstr(cfg_t* s, const char* name) {
    const char* v = cfg_getstr(s, name);
    return v ? std::string(v) : std::string();
}

}  // namespace

bool load_config(const std::string& path, AppConfig& out, std::string& error) {
    cfg_opt_t opts[] = {
        CFG_INT("log_level", 4, CFGF_NONE),
        CFG_STR("log_file",  "", CFGF_NONE),
        CFG_SEC("batery",   battery_opts,  CFGF_MULTI | CFGF_TITLE),
        CFG_SEC("inverter", inverter_opts, CFGF_NONE),
        CFG_SEC("http",     http_opts,     CFGF_NONE),
        CFG_SEC("history",  history_opts,  CFGF_NONE),
        CFG_END()
    };

    cfg_t* cfg = cfg_init(opts, CFGF_NONE);
    cfg_set_validate_func(cfg, "batery|type",     vf_type);
    cfg_set_validate_func(cfg, "batery|protocol", vf_protocol);
    cfg_set_validate_func(cfg, "batery",          vf_battery);

    int rc = cfg_parse(cfg, path.c_str());
    if (rc == CFG_FILE_ERROR) {
        error = "cannot open config file: " + path + " (" + std::strerror(errno) + ")";
        cfg_free(cfg);
        return false;
    }
    if (rc == CFG_PARSE_ERROR) {
        error = "parse/validation error in config file: " + path;
        cfg_free(cfg);
        return false;
    }

    auto fail = [&](std::string msg) {
        error = "config: " + std::move(msg);
        cfg_free(cfg);
        return false;
    };

    out.log_level = cfg_getint(cfg, "log_level");
    out.log_file  = sstr(cfg, "log_file");

    cfg_t* inv = cfg_getsec(cfg, "inverter");
    if (!inv) return fail("inverter { ... } block is required");
    out.inverter.uart_device = sstr(inv, "uart");
    if (out.inverter.uart_device.empty())
        return fail("inverter { uart = ... } is required");
    out.inverter.uart_baud = cfg_getint(inv, "baud");

    if (cfg_t* s = cfg_getsec(cfg, "http")) {
        out.http.http_port = cfg_getint(s, "port");
        out.http.www_root  = sstr(s, "www_root");
    }
    if (cfg_t* s = cfg_getsec(cfg, "history")) {
        out.history.db_path      = sstr(s, "db_path");
        out.history.ram_window_s = cfg_getint(s, "ram_window_s");
    }

    const unsigned n = cfg_size(cfg, "batery");
    if (n == 0) return fail("at least one 'batery' block is required");

    std::set<std::string> seen_names;
    for (unsigned i = 0; i < n; ++i) {
        cfg_t* b = cfg_getnsec(cfg, "batery", i);
        BatteryConfig bc;
        const char* title = cfg_title(b);
        bc.name = title ? title : "";
        if (bc.name.empty())
            return fail("a 'batery' block is missing its name");
        if (!seen_names.insert(bc.name).second)
            return fail("duplicate battery name '" + bc.name + "'");

        bc.link_raw  = sstr(b, "type");
        link_from_str(bc.link_raw, bc.link);
        bc.address   = sstr(b, "address");
        bc.protocol  = sstr(b, "protocol");
        bc.capacity  = cfg_getint(b, "capacity");

        if (bc.address.empty())
            return fail("battery '" + bc.name + "': 'address' is required");
        if (bc.monitored() && bc.protocol.empty())
            return fail("battery '" + bc.name + "': 'protocol' is required");
        if (!bc.monitored() && bc.capacity <= 0)
            return fail("reserve battery '" + bc.name +
                        "': 'capacity' must be > 0");

        bc.service_uuid     = sstr(b, "service_uuid");
        bc.char_write_uuid  = sstr(b, "char_write_uuid");
        bc.char_notify_uuid = sstr(b, "char_notify_uuid");
        bc.scan_timeout_ms  = cfg_getint(b, "scan_timeout_ms");
        bc.adapter_poll_ms  = cfg_getint(b, "adapter_poll_ms");
        bc.mirror            = sstr(b, "mirror");
        bc.uart_baud         = cfg_getint(b, "uart_baud");
        bc.pylontech_address = cfg_getint(b, "pylontech_address");
        bc.reconnect_ms      = cfg_getint(b, "reconnect_ms");
        bc.bms_poll_period_ms           = cfg_getint(b, "bms_poll_period_ms");
        bc.settings_refresh_period_ms   = cfg_getint(b, "settings_refresh_period_ms");
        bc.bms_state_update_interval_ms = cfg_getint(b, "bms_state_update_interval_ms");

        out.batteries.push_back(std::move(bc));
    }

    // Cross-section: a reserve's `mirror` must name another battery that is monitored.
    for (const auto& bc : out.batteries) {
        if (bc.monitored()) continue;
        auto it = std::find_if(out.batteries.begin(), out.batteries.end(),
            [&](const BatteryConfig& o) { return o.name == bc.mirror; });
        if (it == out.batteries.end())
            return fail("reserve battery '" + bc.name + "': mirror '" +
                        bc.mirror + "' is not a known battery");
        if (!it->monitored())
            return fail("reserve battery '" + bc.name + "': mirror '" +
                        bc.mirror + "' is not a monitored battery");
    }

    cfg_free(cfg);
    return true;
}
