#include "jk_protocol_driver.hpp"

#include "dispatcher.hpp"
#include "shared_state.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

void JkProtocol::reset() {
    assembler_.reset();
}

std::vector<uint8_t> JkProtocol::poll_request() {
    std::vector<uint8_t> req;
    JkRequest{kJkCmdCellInfo, 0, 0}.serialize(req);
    return req;
}

std::vector<uint8_t> JkProtocol::settings_request() {
    std::vector<uint8_t> req;
    JkRequest{kJkCmdDeviceInfo, 0, 0}.serialize(req);
    return req;
}

BatteryProtocol::Parsed JkProtocol::feed(const uint8_t* data, size_t len) {
    Parsed out;
    assembler_.push_chunk(data, len);
    while (auto frame = assembler_.take_complete_frame()) {
        const auto type = jk_response_frame_type(frame->data(), frame->size());
        if (!type) continue;

        if (*type == kJkFrameTypeCellInfo) {
            Telemetry t;
            if (parse_cell_info(frame->data(), frame->size(), t.cells, t.pack))
                out.telemetry = std::move(t);
        } else if (*type == kJkFrameTypeDeviceInfo) {
            JkDeviceInfo d;
            if (parse_device_info(frame->data(), frame->size(), d))
                spdlog::debug("bms: device vendor='{}' fw={} hw={} sn={}",
                              d.vendor_id, d.firmware_version,
                              d.hardware_version, d.serial_number);
        } else if (*type == kJkFrameTypeSettings) {
            JkSettings s;
            if (parse_settings(frame->data(), frame->size(), s)) {
                spdlog::debug("bms: settings cells={} ovp={}mV uvp={}mV "
                              "Ichg={}mA Idis={}mA",
                              s.cell_count, s.cell_ovp_mv, s.cell_uvp_mv,
                              s.max_charge_current_ma, s.max_discharge_current_ma);
                out.settings = std::move(s);
            }
        } else {
            spdlog::warn("bms: unknown frame type {:#04x}", *type);
        }
    }
    return out;
}

// ---- JkRs485Slave -------------------------------------------------------

namespace {
constexpr size_t kRxBufLimit = 4096;
constexpr std::chrono::milliseconds kReplyDelay{20};
}

JkRs485Slave::JkRs485Slave(std::string name, uint8_t address,
                                 const SharedState& source,
                                 BatteryTransport& transport)
    : name_(std::move(name)), address_(address),
      source_(source), transport_(transport) {}

void JkRs485Slave::start(Dispatcher& dispatcher) {
    dispatcher_ = &dispatcher;
    dispatcher_->watch(reply_timer_.fd(), [this] { on_reply_tick(); });
}

void JkRs485Slave::stop() {
    if (!dispatcher_) return;
    reply_timer_.disarm();
    dispatcher_->unwatch(reply_timer_.fd());
    dispatcher_ = nullptr;
}

void JkRs485Slave::on_reply_tick() {
    reply_timer_.consume();
    if (reply_.empty()) return;
    spdlog::debug("battery '{}': answer of {} bytes", name_, reply_.size());
    transport_.send(reply_.data(), reply_.size());
    reply_.clear();
}

void JkRs485Slave::on_connected() {
    spdlog::info("battery '{}': UART up", name_);
}

void JkRs485Slave::on_disconnected() {
    spdlog::warn("battery '{}': UART down", name_);
    rx_buf_.clear();
    reply_timer_.disarm();
    reply_.clear();
}

void JkRs485Slave::on_bytes(const uint8_t* data, size_t len) {
    // The link also carries the master's own frames and its polls of other
    // addresses; everything that is not a poll is skipped byte by byte.
    if (rx_buf_.size() + len > kRxBufLimit) rx_buf_.clear();
    rx_buf_.insert(rx_buf_.end(), data, data + len);

    size_t pos = 0;
    while (pos < rx_buf_.size()) {
        JkRs485Request req;
        const int n = try_parse_jk_rs485_request(rx_buf_.data() + pos,
                                                    rx_buf_.size() - pos, req);
        if (n == 0) break;
        if (n < 0) { pos += static_cast<size_t>(-n); continue; }
        pos += static_cast<size_t>(n);

        if (req.address != address_) continue;

        const auto type = jk_rs485_frame_type(req.reg);
        if (!type) {
            spdlog::debug("battery '{}': poll of register {:#06x} unsupported",
                          name_, req.reg);
            continue;
        }
        if (!source_.is_fresh(std::chrono::seconds(5))) {
            spdlog::debug("battery '{}': no live data to relay, silent", name_);
            continue;
        }
        // The frame is put together from the parsed data, so the source
        // may speak any protocol.
        const auto snap = source_.snapshot();
        std::vector<uint8_t> frame;
        if (*type == kJkFrameTypeCellInfo && snap.cells && snap.pack)
            frame = build_cell_info_frame(*snap.cells, *snap.pack);
        else if (*type == kJkFrameTypeSettings && snap.settings)
            frame = build_settings_frame(*snap.settings);
        if (frame.empty()) {
            spdlog::debug("battery '{}': nothing to answer poll {:#06x} with",
                          name_, req.reg);
            continue;
        }
        // Polls come 200 ms apart, so one answer at a time is pending.
        reply_ = build_jk_rs485_response(address_, req.reg, frame);
        reply_timer_.arm(kReplyDelay);
    }
    rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + static_cast<long>(pos));
}
