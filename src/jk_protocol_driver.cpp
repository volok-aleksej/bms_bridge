#include "jk_protocol_driver.hpp"

#include <spdlog/spdlog.h>

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
