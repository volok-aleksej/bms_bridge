#pragma once

#include "shared_state.hpp"
#include "timer.hpp"

#include <cstdint>
#include <string>
#include <vector>

class Dispatcher;

class Inverter {
public:
    Inverter(std::string device, int baud, SharedState& state);
    ~Inverter();

    Inverter(const Inverter&) = delete;
    Inverter& operator=(const Inverter&) = delete;

    // Wire the inverter into the given dispatcher's epoll loop. Must be
    // called once, before dispatcher.run(). The dispatcher must outlive
    // this Inverter.
    void attach(Dispatcher& d);

private:
    int  open_uart();
    void try_open();
    void close_uart();
    void on_uart_readable();
    void on_reopen_tick();
    void on_rx(const uint8_t* data, size_t len);
    void process_buffer();
    bool send_response(const std::vector<uint8_t>& bytes);

    std::string device_;
    int baud_ = 115200;
    static constexpr uint8_t kBaseAddress = 2;
    SharedState& state_;

    std::vector<uint8_t> rx_buf_;

    Dispatcher* dispatcher_ = nullptr;
    int   uart_fd_ = -1;
    Timer reopen_timer_;
};
