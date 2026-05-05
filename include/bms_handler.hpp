#pragma once

#include "ble_client.hpp"
#include "event_queue.hpp"
#include "history.hpp"
#include "jk_protocol.hpp"
#include "shared_state.hpp"

#include <chrono>
#include <cstdint>

class BmsHandler {
public:
    BmsHandler(BleClient& ble, BmsEventQueue& queue,
               SharedState& state, History& history,
               std::chrono::milliseconds history_interval,
               std::chrono::milliseconds state_update_interval);

    // Called when queue.notify_fd() becomes readable.
    void on_queue_event();

    // Called by the periodic cell_info poll timer.
    void on_poll_tick();

    // Called by the periodic device_info / settings refresh timer.
    void on_settings_tick();

private:
    void send_request(uint8_t cmd);
    void on_connected();
    void on_disconnected();
    void on_notify_frame(const BmsEvent& ev);
    void on_cell_info(JkCellInfo cells, JkPackInfo pack);
    void on_device_info(const uint8_t* data, size_t len);
    void on_settings_frame(const uint8_t* data, size_t len);

    BleClient&     ble_;
    BmsEventQueue& queue_;
    SharedState&   state_;
    History&       history_;

    std::chrono::milliseconds             history_interval_;
    std::chrono::milliseconds             state_update_interval_;
    std::chrono::steady_clock::time_point last_history_write_{};
    std::chrono::steady_clock::time_point last_state_update_{};

    JkFrameAssembler jk_assembler_;
};
