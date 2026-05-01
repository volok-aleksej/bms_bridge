#pragma once

#include "shared_state.hpp"
#include "thread.hpp"

#include <cstdint>
#include <string>
#include <vector>

class Inverter : public Thread {
public:
    Inverter(std::string device, int baud, uint8_t pylontech_address,
             SharedState& state);
    ~Inverter() override;

protected:
    void run() override;
    void on_stop() override;

private:
    int open_uart();
    void on_rx(const uint8_t* data, size_t len);
    void process_buffer();
    bool send_response(const std::vector<uint8_t>& bytes);

    std::string device_;
    int baud_ = 115200;
    uint8_t pylontech_address_ = 2;
    SharedState& state_;

    std::vector<uint8_t> rx_buf_;

    int uart_fd_ = -1;
    int epfd_ = -1;
    int stop_evfd_ = -1;
};
