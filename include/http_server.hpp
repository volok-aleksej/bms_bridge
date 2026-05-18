#pragma once

#include "history.hpp"
#include "thread.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct event_base;
struct evhttp;
struct evhttp_request;
struct event;
struct evkeyvalq;

class Battery;
class SharedState;

class HttpServer : public Thread {
public:
    HttpServer(const History& history,
               const std::vector<std::unique_ptr<Battery>>& batteries,
               uint16_t port, std::string www_root);
    ~HttpServer() override;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

protected:
    void run() override;
    void on_stop() override;

private:
    struct PageCtx {
        int                                    battery_id = 0;
        std::chrono::system_clock::time_point  time_start;
        std::chrono::system_clock::time_point  time_end;
        int                                    count      = 0;
        std::chrono::steady_clock::time_point  expires_at;
    };

    static void on_request(struct evhttp_request* req, void* self);
    static void on_sweep(int, short, void* self);
    void handle_info(struct evhttp_request* req);
    void handle_batteries(struct evhttp_request* req);
    void handle_history(struct evhttp_request* req);
    void serve_file(struct evhttp_request* req, const std::string& path);
    void handle_history_next(struct evhttp_request* req, const std::string& req_id);
    void handle_history_initial(struct evhttp_request* req,
                                const struct evkeyvalq& params);

    const SharedState* state_for(const std::string& battery_id) const;
    static int battery_param(struct evhttp_request* req);
    static void send_json(struct evhttp_request* req, int code, std::string body);
    static std::string gen_uuid4();

    const History&                                history_;
    const std::vector<std::unique_ptr<Battery>>&  batteries_;
    uint16_t                                      port_;
    std::string                                   www_root_;

    event_base* base_     = nullptr;
    evhttp*     http_     = nullptr;
    event*      sweep_ev_ = nullptr;

    std::unordered_map<std::string, PageCtx> ctxs_;
};
