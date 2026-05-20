#include "http_server.hpp"

#include "battery.hpp"
#include "shared_state.hpp"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include <cjson/cJSON.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits.h>
#include <memory>
#include <random>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <vector>

namespace {

constexpr int kCtxTtlSec  = 300;
constexpr int kSweepSec   =  60;

// Battery ids found in the DB but not in the current config are pre-multi-
// battery ("legacy") data. The old single-battery bridge was always a JK
// BMS over BLE, so that is what those records actually are.
constexpr const char* kLegacyLink     = "bluetooth";
constexpr const char* kLegacyProtocol = "jk_bt";

struct CJsonPtr {
    cJSON* p;
    explicit CJsonPtr(cJSON* p) : p(p) {}
    ~CJsonPtr() { cJSON_Delete(p); }
    CJsonPtr(const CJsonPtr&) = delete;
    CJsonPtr& operator=(const CJsonPtr&) = delete;
};

std::string cjson_print(cJSON* obj) {
    char* raw = cJSON_PrintUnformatted(obj);
    if (!raw) return {};
    std::string out(raw);
    cJSON_free(raw);
    return out;
}

cJSON* sample_to_cjson(const TelemetrySample& s) {
    const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              s.ts.time_since_epoch()).count();

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "ts",                (double)ts_ms);
    cJSON_AddNumberToObject(obj, "voltage_mv",         s.pack.voltage_mv);
    cJSON_AddNumberToObject(obj, "current_ma",         s.pack.current_ma);
    cJSON_AddNumberToObject(obj, "soc_pct",            s.pack.state_of_charge_pct);
    cJSON_AddNumberToObject(obj, "remaining_mah",      s.pack.remaining_capacity_mah);
    cJSON_AddNumberToObject(obj, "total_mah",          s.pack.total_capacity_mah);
    cJSON_AddNumberToObject(obj, "cycle_count",        s.pack.cycle_count);
    cJSON_AddNumberToObject(obj, "temp1_dc",           s.pack.battery_temp1_dC);
    cJSON_AddNumberToObject(obj, "temp2_dc",           s.pack.battery_temp2_dC);
    cJSON_AddNumberToObject(obj, "mos_temp_dc",        s.pack.power_tube_temp_dC);
    cJSON_AddBoolToObject  (obj, "charging",           s.pack.charging_enabled);
    cJSON_AddBoolToObject  (obj, "discharging",        s.pack.discharging_enabled);
    cJSON_AddBoolToObject  (obj, "balancer",           s.pack.balancer_enabled);
    cJSON_AddNumberToObject(obj, "balance_current_ma", s.pack.balance_current_ma);
    cJSON_AddNumberToObject(obj, "errors",             s.pack.errors_bitmask);
    cJSON_AddNumberToObject(obj, "avg_cell_mv",        s.cells.average_voltage_mv);
    cJSON_AddNumberToObject(obj, "diff_cell_mv",       s.cells.voltage_diff_mv);

    cJSON* cells_mv = cJSON_CreateArray();
    for (uint16_t v : s.cells.voltages_mv)
        cJSON_AddItemToArray(cells_mv, cJSON_CreateNumber(v));
    cJSON_AddItemToObject(obj, "cells_mv", cells_mv);

    cJSON* res_uohm = cJSON_CreateArray();
    for (uint16_t v : s.cells.resistance_uohm)
        cJSON_AddItemToArray(res_uohm, cJSON_CreateNumber(v));
    cJSON_AddItemToObject(obj, "resistance_uohm", res_uohm);

    return obj;
}

std::string history_json(const std::string& req_id,
                         const std::vector<TelemetrySample>& data) {
    CJsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.p, "req_id", req_id.c_str());

    cJSON* arr = cJSON_AddArrayToObject(root.p, "data");
    for (const auto& s : data)
        cJSON_AddItemToArray(arr, sample_to_cjson(s));

    return cjson_print(root.p);
}

const char* mime_for_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")                   return "text/css";
    if (ext == ".js")                    return "application/javascript";
    if (ext == ".json")                  return "application/json";
    if (ext == ".svg")                   return "image/svg+xml";
    if (ext == ".png")                   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")                   return "image/x-icon";
    if (ext == ".woff")                  return "font/woff";
    if (ext == ".woff2")                 return "font/woff2";
    if (ext == ".ttf")                   return "font/ttf";
    if (ext == ".txt")                   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

} // namespace

HttpServer::HttpServer(const History& history,
                       const std::vector<std::unique_ptr<Battery>>& batteries,
                       uint16_t port, std::string www_root)
    : history_(history), batteries_(batteries), port_(port),
      www_root_(std::move(www_root)) {
    evthread_use_pthreads();
    base_ = event_base_new();
    if (!base_) throw std::runtime_error("http: event_base_new failed");

    http_ = evhttp_new(base_);
    if (!http_) throw std::runtime_error("http: evhttp_new failed");

    evhttp_set_gencb(http_, on_request, this);

    if (evhttp_bind_socket(http_, "0.0.0.0", port_) != 0)
        throw std::runtime_error("http: cannot bind port " + std::to_string(port_));

    sweep_ev_ = event_new(base_, -1, EV_PERSIST, on_sweep, this);
    struct timeval tv{kSweepSec, 0};
    event_add(sweep_ev_, &tv);
}

HttpServer::~HttpServer() {
    stop();
    if (sweep_ev_) { event_free(sweep_ev_); sweep_ev_ = nullptr; }
    if (http_)     { evhttp_free(http_);    http_     = nullptr; }
    if (base_)     { event_base_free(base_); base_    = nullptr; }
}

void HttpServer::run() {
    spdlog::info("http: listening on 0.0.0.0:{}", port_);
    event_base_dispatch(base_);
    spdlog::info("http: event loop stopped");
}

void HttpServer::on_stop() {
    event_base_loopbreak(base_);
}

void HttpServer::on_request(evhttp_request* req, void* arg) {
    auto* self = static_cast<HttpServer*>(arg);
    const char* uri = evhttp_request_get_uri(req);

    evhttp_uri* parsed = evhttp_uri_parse(uri);
    if (!parsed) {
        evhttp_send_error(req, HTTP_BADREQUEST, "Bad URI");
        return;
    }
    const char* path = evhttp_uri_get_path(parsed);
    std::string path_s = path ? path : "/";
    evhttp_uri_free(parsed);

    if (path_s == "/") path_s = "/index.html";

    if (path_s == "/info") {
        self->handle_info(req);
    } else if (path_s == "/batteries") {
        self->handle_batteries(req);
    } else if (path_s == "/history") {
        self->handle_history(req);
    } else if (path_s == "/daily") {
        self->handle_daily(req);
    } else {
        self->serve_file(req, path_s);
    }
}


void HttpServer::serve_file(evhttp_request* req, const std::string& path) {
    const std::string full = www_root_ + path;

    char resolved[PATH_MAX];
    if (!realpath(full.c_str(), resolved)) {
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }

    char root_resolved[PATH_MAX];
    if (!realpath(www_root_.c_str(), root_resolved)) {
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }

    const std::string root_s = std::string(root_resolved) + '/';
    if (std::string(resolved).compare(0, root_s.size(), root_s) != 0) {
        spdlog::warn("http: path traversal attempt: {}", path);
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }

    std::ifstream f(resolved, std::ios::binary);
    if (!f) {
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }

    std::string body((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, body.data(), body.size());
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", mime_for_ext(resolved));
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
    evbuffer_free(buf);
}

void HttpServer::send_json(evhttp_request* req, int code, std::string body) {
    evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, body.data(), body.size());
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "application/json");
    evhttp_send_reply(req, code, code == HTTP_OK ? "OK" : "Error", buf);
    evbuffer_free(buf);
}

std::string HttpServer::gen_uuid4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> d32;
    std::uniform_int_distribution<uint16_t> d16;

    uint32_t a  = d32(rng);
    uint16_t b  = d16(rng);
    uint16_t c  = static_cast<uint16_t>((d16(rng) & 0x0FFFu) | 0x4000u); // version 4
    uint16_t d  = static_cast<uint16_t>((d16(rng) & 0x3FFFu) | 0x8000u); // variant 1
    uint32_t e1 = d32(rng);
    uint16_t e2 = d16(rng);

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%08x%04x",
             a, b, c, d, e1, e2);
    return buf;
}

void HttpServer::on_sweep(int, short, void* arg) {
    auto* self = static_cast<HttpServer*>(arg);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = self->ctxs_.begin(); it != self->ctxs_.end(); ) {
        if (now >= it->second.expires_at) {
            spdlog::debug("http: evict stale pagination ctx {}", it->first);
            it = self->ctxs_.erase(it);
        } else {
            ++it;
        }
    }
}

const SharedState* HttpServer::state_for(const std::string& battery_id) const {
    for (const auto& b : batteries_)
        if (b->name() == battery_id) return &b->state();
    return nullptr;
}

int HttpServer::battery_param(evhttp_request* req) {
    const char* uri = evhttp_request_get_uri(req);
    int out = 0;
    evhttp_uri* parsed = evhttp_uri_parse(uri);
    const char* q = parsed ? evhttp_uri_get_query(parsed) : nullptr;
    evkeyvalq params{};
    if (q) evhttp_parse_query_str(q, &params);
    if (const char* b = evhttp_find_header(&params, "battery")) out = std::atoi(b);
    evhttp_clear_headers(&params);
    if (parsed) evhttp_uri_free(parsed);
    return out;
}

void HttpServer::handle_batteries(evhttp_request* req) {
    CJsonPtr root(cJSON_CreateArray());

    // The list is the `batteries` table (source of truth) .
    // A configured battery only appears once
    // it has its first sample (its row is lazily created on append);
    // rows whose name is not in the current config are legacy/removed data.
    for (const auto& ref : history_.battery_list()) {
        const Battery* cfg = nullptr;
        for (const auto& b : batteries_)
            if (b->config().name == ref.name) { cfg = b.get(); break; }

        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id",        ref.id);
        cJSON_AddStringToObject(o, "name",      ref.name.c_str());
        if (cfg) {
            const BatteryConfig& c = cfg->config();
            cJSON_AddStringToObject(o, "link",      c.link_raw.c_str());
            cJSON_AddStringToObject(o, "protocol",  c.protocol.c_str());
            cJSON_AddBoolToObject  (o, "monitored", cfg->monitored());
            cJSON_AddNumberToObject(o, "capacity",  c.capacity);
            cJSON_AddBoolToObject  (o, "legacy",    false);
        } else {
            cJSON_AddStringToObject(o, "link",      kLegacyLink);
            cJSON_AddStringToObject(o, "protocol",  kLegacyProtocol);
            cJSON_AddBoolToObject  (o, "monitored", false);
            cJSON_AddNumberToObject(o, "capacity",  0);
            cJSON_AddBoolToObject  (o, "legacy",    true);
        }
        cJSON_AddItemToArray(root.p, o);
    }

    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_info(evhttp_request* req) {
    const std::string name = history_.battery_name(battery_param(req));

    const Battery* bat = nullptr;
    for (const auto& b : batteries_)
        if (b->config().name == name) { bat = b.get(); break; }

    if (!bat) {
        send_json(req, HTTP_NOTFOUND, "{\"error\":\"battery not in config\"}");
        return;
    }

    // Reserve battery: no live BMS — its "settings" are what the bridge
    // synthesises for the inverter from the config (own capacity, a 0.5C
    // charge/discharge limit, SoC pinned at 100 %, zero current).
    if (!bat->config().monitored()) {
        const BatteryConfig& c = bat->config();
        constexpr int kReserveCRateTenths = 5;  // 0.5C (matches inverter.cpp)
        const uint32_t cap_mah  = static_cast<uint32_t>(c.capacity) * 1000;
        const uint32_t limit_ma = static_cast<uint32_t>(c.capacity) * 1000
                                  * kReserveCRateTenths / 10;
        CJsonPtr ro(cJSON_CreateObject());
        cJSON_AddBoolToObject  (ro.p, "reserve",                    true);
        cJSON_AddStringToObject(ro.p, "mirror",                     c.mirror.c_str());
        cJSON_AddNumberToObject(ro.p, "capacity_mah",               cap_mah);
        cJSON_AddNumberToObject(ro.p, "charge_current_limit_ma",    limit_ma);
        cJSON_AddNumberToObject(ro.p, "discharge_current_limit_ma", limit_ma);
        cJSON_AddNumberToObject(ro.p, "soc_pct",                    100);
        cJSON_AddNumberToObject(ro.p, "current_ma",                 0);
        send_json(req, HTTP_OK, cjson_print(ro.p));
        return;
    }

    const SharedState* st = state_for(name);
    if (!st) {
        send_json(req, HTTP_NOTFOUND,
                  "{\"error\":\"no live data for this battery\"}");
        return;
    }
    const auto snap = st->snapshot();
    if (!snap.settings) {
        send_json(req, HTTP_NOTFOUND, "{\"error\":\"settings not yet received\"}");
        return;
    }
    const JkSettings& s = *snap.settings;
    CJsonPtr root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.p, "cell_count",                s.cell_count);
    cJSON_AddNumberToObject(root.p, "nominal_capacity_mah",      s.nominal_capacity_mah);
    cJSON_AddNumberToObject(root.p, "cell_uvp_mv",               s.cell_uvp_mv);
    cJSON_AddNumberToObject(root.p, "cell_uvpr_mv",              s.cell_uvpr_mv);
    cJSON_AddNumberToObject(root.p, "cell_ovp_mv",               s.cell_ovp_mv);
    cJSON_AddNumberToObject(root.p, "cell_ovpr_mv",              s.cell_ovpr_mv);
    cJSON_AddNumberToObject(root.p, "soc_100_mv",                s.soc_100_mv);
    cJSON_AddNumberToObject(root.p, "soc_0_mv",                  s.soc_0_mv);
    cJSON_AddNumberToObject(root.p, "balance_trigger_mv",        s.balance_trigger_mv);
    cJSON_AddNumberToObject(root.p, "start_balance_mv",          s.start_balance_mv);
    cJSON_AddNumberToObject(root.p, "max_charge_current_ma",     s.max_charge_current_ma);
    cJSON_AddNumberToObject(root.p, "max_discharge_current_ma",  s.max_discharge_current_ma);
    cJSON_AddNumberToObject(root.p, "max_balance_current_ma",    s.max_balance_current_ma);
    cJSON_AddNumberToObject(root.p, "charge_ocp_delay_s",        s.charge_ocp_delay_s);
    cJSON_AddNumberToObject(root.p, "charge_ocp_recovery_s",     s.charge_ocp_recovery_s);
    cJSON_AddNumberToObject(root.p, "discharge_ocp_delay_s",     s.discharge_ocp_delay_s);
    cJSON_AddNumberToObject(root.p, "discharge_ocp_recovery_s",  s.discharge_ocp_recovery_s);
    cJSON_AddNumberToObject(root.p, "scp_delay_us",              s.scp_delay_us);
    cJSON_AddNumberToObject(root.p, "scp_recovery_s",            s.scp_recovery_s);
    cJSON_AddNumberToObject(root.p, "charge_otp_dC",             s.charge_otp_dC);
    cJSON_AddNumberToObject(root.p, "charge_otp_recovery_dC",    s.charge_otp_recovery_dC);
    cJSON_AddNumberToObject(root.p, "discharge_otp_dC",          s.discharge_otp_dC);
    cJSON_AddNumberToObject(root.p, "discharge_otp_recovery_dC", s.discharge_otp_recovery_dC);
    cJSON_AddNumberToObject(root.p, "charge_utp_dC",             s.charge_utp_dC);
    cJSON_AddNumberToObject(root.p, "charge_utp_recovery_dC",    s.charge_utp_recovery_dC);
    cJSON_AddNumberToObject(root.p, "mosfet_otp_dC",             s.mosfet_otp_dC);
    cJSON_AddNumberToObject(root.p, "mosfet_otp_recovery_dC",    s.mosfet_otp_recovery_dC);
    cJSON_AddNumberToObject(root.p, "smart_sleep_mv",            s.smart_sleep_mv);
    cJSON_AddNumberToObject(root.p, "power_off_mv",              s.power_off_mv);
    cJSON_AddNumberToObject(root.p, "request_charge_mv",         s.request_charge_mv);
    cJSON_AddNumberToObject(root.p, "request_float_mv",          s.request_float_mv);
    cJSON_AddBoolToObject  (root.p, "charging_switch_on",        s.charging_switch_on);
    cJSON_AddBoolToObject  (root.p, "discharging_switch_on",     s.discharging_switch_on);
    cJSON_AddBoolToObject  (root.p, "balancer_switch_on",        s.balancer_switch_on);
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_daily(evhttp_request* req) {
    const char* uri = evhttp_request_get_uri(req);
    evhttp_uri* parsed = evhttp_uri_parse(uri);
    const char* q = parsed ? evhttp_uri_get_query(parsed) : nullptr;
    evkeyvalq params{};
    if (q) evhttp_parse_query_str(q, &params);

    int battery_id = 0, year = 0, month = 0;
    if (const char* v = evhttp_find_header(&params, "battery")) battery_id = std::atoi(v);
    if (const char* v = evhttp_find_header(&params, "year"))    year  = std::atoi(v);
    if (const char* v = evhttp_find_header(&params, "month"))   month = std::atoi(v);
    evhttp_clear_headers(&params);
    if (parsed) evhttp_uri_free(parsed);

    if (year <= 0 || month <= 0) {
        time_t now_t = std::chrono::system_clock::to_time_t(
                           std::chrono::system_clock::now());
        struct tm gmt{};
        gmtime_r(&now_t, &gmt);
        year  = gmt.tm_year + 1900;
        month = gmt.tm_mon + 1;
    }

    const auto data = history_.get_daily_energy(battery_id, year, month);

    CJsonPtr root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.p, "battery_id", battery_id);
    cJSON_AddNumberToObject(root.p, "year",  year);
    cJSON_AddNumberToObject(root.p, "month", month);
    cJSON* arr = cJSON_AddArrayToObject(root.p, "data");
    for (const auto& e : data) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "date",          e.date.c_str());
        cJSON_AddNumberToObject(o, "charged_wh",    e.charged_wh);
        cJSON_AddNumberToObject(o, "discharged_wh", e.discharged_wh);
        cJSON_AddItemToArray(arr, o);
    }
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_history(evhttp_request* req) {
    const char* uri = evhttp_request_get_uri(req);

    evhttp_uri* parsed = evhttp_uri_parse(uri);
    const char* query_cstr = parsed ? evhttp_uri_get_query(parsed) : nullptr;

    evkeyvalq params{};
    if (query_cstr) evhttp_parse_query_str(query_cstr, &params);
    if (parsed) evhttp_uri_free(parsed);

    const char* next_param   = evhttp_find_header(&params, "next");
    const char* req_id_param = evhttp_find_header(&params, "req_id");

    if (next_param != nullptr) {
        if (!req_id_param || req_id_param[0] == '\0') {
            evhttp_clear_headers(&params);
            send_json(req, HTTP_BADREQUEST,
                      "{\"error\":\"req_id required for ?next\"}");
            return;
        }
        std::string req_id = req_id_param;
        evhttp_clear_headers(&params);
        handle_history_next(req, req_id);
    } else {
        handle_history_initial(req, params);
        evhttp_clear_headers(&params);
    }
}

void HttpServer::handle_history_next(evhttp_request* req,
                                     const std::string& req_id) {
    auto it = ctxs_.find(req_id);
    if (it == ctxs_.end()) {
        send_json(req, HTTP_NOTFOUND, "{\"error\":\"req_id not found\"}");
        return;
    }
    PageCtx& ctx = it->second;
    const auto win_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            ctx.time_end - ctx.time_start).count();
    const int64_t step_ms = (win_ms > 0 && ctx.count > 0) ? win_ms / ctx.count : 0;
    auto data = history_.range_by_id(ctx.battery_id, ctx.time_start,
                                     ctx.time_end, ctx.count, step_ms);

    if (!data.empty()) {
        const auto probe = history_.range_by_id(ctx.battery_id, ctx.time_start,
                                                data.back().ts, 1, 0);
        if (!probe.empty()) {
            ctx.time_end   = data.back().ts;
            ctx.expires_at = std::chrono::steady_clock::now()
                             + std::chrono::seconds(kCtxTtlSec);
        } else {
            ctxs_.erase(it);
        }
    } else {
        ctxs_.erase(it);
    }

    send_json(req, HTTP_OK, history_json(req_id, data));
}

void HttpServer::handle_history_initial(evhttp_request* req,
                                        const evkeyvalq& params) {
    const char* ts_start_p = evhttp_find_header(&params, "time_start");
    const char* ts_end_p   = evhttp_find_header(&params, "time_end");
    const char* count_p    = evhttp_find_header(&params, "count");
    const char* battery_p  = evhttp_find_header(&params, "battery");
    const int   bid        = battery_p ? std::atoi(battery_p) : 0;

    const auto now = std::chrono::system_clock::now();
    auto time_start = now - std::chrono::hours(24);
    auto time_end   = now;
    int  count      = 100;

    if (ts_start_p && ts_start_p[0])
        time_start = std::chrono::system_clock::from_time_t(
                         static_cast<time_t>(std::atoll(ts_start_p)));
    if (ts_end_p && ts_end_p[0])
        time_end = std::chrono::system_clock::from_time_t(
                       static_cast<time_t>(std::atoll(ts_end_p)));
    if (count_p && count_p[0]) {
        int v = std::atoi(count_p);
        if (v > 0) count = v;
    }

    const auto win_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            time_end - time_start).count();
    const int64_t step_ms = (win_ms > 0 && count > 0) ? win_ms / count : 0;
    auto data = history_.range_by_id(bid, time_start, time_end, count, step_ms);
    std::string req_id = gen_uuid4();

    if (!data.empty()) {
        const auto probe = history_.range_by_id(bid, time_start,
                                                data.back().ts, 1, 0);
        if (!probe.empty()) {
            const auto expires = std::chrono::steady_clock::now()
                                 + std::chrono::seconds(kCtxTtlSec);
            ctxs_[req_id] = PageCtx{bid, time_start, data.back().ts,
                                    count, expires};
        }
    }

    send_json(req, HTTP_OK, history_json(req_id, data));
}
