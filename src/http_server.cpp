#include "http_server.hpp"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kCtxTtlSec  = 300; // pagination context TTL: 5 minutes
constexpr int kSweepSec   =  60; // sweep interval

std::string json_str(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    o += '"';
    return o;
}

std::string sample_to_json(const TelemetrySample& s) {
    const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              s.ts.time_since_epoch()).count();
    std::ostringstream o;
    o << "{"
      << "\"ts\":"           << ts_ms
      << ",\"voltage_mv\":"  << s.pack.voltage_mv
      << ",\"current_ma\":"  << s.pack.current_ma
      << ",\"soc_pct\":"     << static_cast<int>(s.pack.state_of_charge_pct)
      << ",\"remaining_mah\":" << s.pack.remaining_capacity_mah
      << ",\"total_mah\":"   << s.pack.total_capacity_mah
      << ",\"cycle_count\":" << s.pack.cycle_count
      << ",\"temp1_dc\":"    << s.pack.battery_temp1_dC
      << ",\"temp2_dc\":"    << s.pack.battery_temp2_dC
      << ",\"mos_temp_dc\":" << s.pack.power_tube_temp_dC
      << ",\"charging\":"    << (s.pack.charging_enabled    ? "true" : "false")
      << ",\"discharging\":" << (s.pack.discharging_enabled ? "true" : "false")
      << ",\"balancer\":"    << (s.pack.balancer_enabled    ? "true" : "false")
      << ",\"errors\":"      << s.pack.errors_bitmask
      << ",\"avg_cell_mv\":" << s.cells.average_voltage_mv
      << ",\"diff_cell_mv\":" << s.cells.voltage_diff_mv
      << ",\"cells_mv\":[";
    for (size_t i = 0; i < s.cells.voltages_mv.size(); ++i) {
        if (i) o << ',';
        o << s.cells.voltages_mv[i];
    }
    o << "],\"resistance_uohm\":[";
    for (size_t i = 0; i < s.cells.resistance_uohm.size(); ++i) {
        if (i) o << ',';
        o << s.cells.resistance_uohm[i];
    }
    o << "]}";
    return o.str();
}

std::string history_json(const std::string& req_id,
                         const std::vector<TelemetrySample>& data) {
    std::ostringstream o;
    o << "{\"req_id\":" << json_str(req_id) << ",\"data\":[";
    for (size_t i = 0; i < data.size(); ++i) {
        if (i) o << ',';
        o << sample_to_json(data[i]);
    }
    o << "]}";
    return o.str();
}

} // namespace

HttpServer::HttpServer(const History& history, uint16_t port, std::string www_root)
    : history_(history), port_(port), www_root_(std::move(www_root)) {
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
    stop(); // join thread before freeing libevent resources
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

    if (path_s == "/") {
        self->handle_root(req);
    } else if (path_s == "/history") {
        self->handle_history(req);
    } else {
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
    }
}

void HttpServer::handle_root(evhttp_request* req) {
    const std::string path = www_root_ + "/index.html";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        spdlog::warn("http: cannot open {}", path);
        evhttp_send_error(req, HTTP_NOTFOUND, "index.html not found");
        return;
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, body.data(), body.size());
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "text/html; charset=utf-8");
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
    auto data = history_.range(ctx.time_start, ctx.time_end, ctx.count + 1, step_ms);

    if (static_cast<int>(data.size()) > ctx.count) {
        ctx.time_end    = data[ctx.count - 1].ts;
        ctx.expires_at  = std::chrono::steady_clock::now()
                          + std::chrono::seconds(kCtxTtlSec);
        data.resize(ctx.count);
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
    auto data = history_.range(time_start, time_end, count + 1, step_ms);
    std::string req_id = gen_uuid4();

    if (static_cast<int>(data.size()) > count) {
        const auto expires = std::chrono::steady_clock::now()
                             + std::chrono::seconds(kCtxTtlSec);
        ctxs_[req_id] = PageCtx{time_start, data[count - 1].ts, count, expires};
        data.resize(count);
    }

    send_json(req, HTTP_OK, history_json(req_id, data));
}
