#pragma once

#include "history.hpp"
#include "thread.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

struct event_base;
struct evhttp;
struct evhttp_request;
struct event;

class HttpServer : public Thread {
public:
    HttpServer(const History& history, uint16_t port, std::string www_root);
    ~HttpServer() override;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

protected:
    void run() override;
    void on_stop() override;

private:
    struct PageCtx {
        std::chrono::system_clock::time_point  time_start;
        std::chrono::system_clock::time_point  time_end;
        int                                    count      = 0;
        std::chrono::steady_clock::time_point  expires_at;
    };

    static void on_request(struct evhttp_request* req, void* self);
    static void on_sweep(int, short, void* self);
    void handle_root(struct evhttp_request* req);
    void handle_history(struct evhttp_request* req);
    void handle_history_next(struct evhttp_request* req, const std::string& req_id);
    void handle_history_initial(struct evhttp_request* req,
                                const struct evkeyvalq& params);

    static void send_json(struct evhttp_request* req, int code, std::string body);

    static std::string gen_uuid4();

    const History& history_;
    uint16_t       port_;
    std::string    www_root_;

    event_base* base_     = nullptr;
    evhttp*     http_     = nullptr;
    event*      sweep_ev_ = nullptr;

    std::unordered_map<std::string, PageCtx> ctxs_;
};
