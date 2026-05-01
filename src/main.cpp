#include "ble_client.hpp"
#include "config.hpp"
#include "dispatcher.hpp"
#include "event_queue.hpp"
#include "inverter.hpp"
#include "jk_protocol.hpp"
#include "logger.hpp"
#include "shared_state.hpp"

#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-D level] [-U device_uuid]\n"
              << "  -Dx   log verbosity 1..5 (1=error, 2=warning, 3=info, 4=debug, 5=trace)\n"
              << "  -Uxxx BLE address/identifier of the device to connect to\n";
}

constexpr const char* kDefaultConfigPath = "bms_bridge.conf";

}  // namespace

int main(int argc, char** argv) {
    std::string cli_uuid;
    int cli_log_level = -1;

    int opt;
    while ((opt = getopt(argc, argv, "D:U:h")) != -1) {
        switch (opt) {
            case 'D':
                cli_log_level = std::atoi(optarg);
                if (cli_log_level < 1 || cli_log_level > 5) {
                    std::cerr << "log level must be in [1..5]\n";
                    return EXIT_FAILURE;
                }
                break;
            case 'U':
                cli_uuid = optarg;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    const char* env_path = std::getenv("BMS_BRIDGE_CONFIG");
    std::string cfg_path = env_path ? env_path : kDefaultConfigPath;

    AppConfig cfg;
    std::string err;
    if (!load_config(cfg_path, cfg, err)) {
        std::cerr << "config error: " << err << "\n";
        return EXIT_FAILURE;
    }

    if (cli_log_level > 0) cfg.log_level = cli_log_level;
    if (!cli_uuid.empty()) cfg.device_uuid = cli_uuid;

    init_logger(cfg.log_level, cfg.log_file);

    if (cfg.device_uuid.empty() || cfg.device_uuid == "00:00:00:00:00:00") {
        spdlog::critical("device uuid is not configured (use -U or set device_uuid in config)");
        return EXIT_FAILURE;
    }

    spdlog::info("bms_bridge starting; bms={} uart={}@{} pylontech_addr={:#04x}",
                 cfg.device_uuid, cfg.uart_device, cfg.uart_baud,
                 cfg.pylontech_address);

    SharedState state;
    BmsEventQueue queue;
    Dispatcher dispatcher;

    BleClient::Settings ble_settings{
        cfg.device_uuid,
        cfg.service_uuid,
        cfg.char_write_uuid,
        cfg.char_notify_uuid,
        cfg.scan_timeout_ms,
        cfg.adapter_poll_ms,
        cfg.reconnect_backoff_ms,
    };
    BleClient ble(ble_settings, queue);
    Inverter  inv(cfg.uart_device, cfg.uart_baud,
                  static_cast<uint8_t>(cfg.pylontech_address), state);

    // Owned by the dispatcher thread only — single-threaded access by design.
    JkFrameAssembler jk_assembler;

    auto send_jk_request = [&](uint8_t cmd) {
        std::vector<uint8_t> req;
        JkRequest{cmd, 0, 0}.serialize(req);
        if (!ble.send(req.data(), req.size())) {
            spdlog::trace("dispatcher: BLE send skipped (cmd {:#04x}, not connected)", cmd);
        }
    };

    // BMS events from queue → SharedState. Single writer (this lambda).
    dispatcher.watch(queue.notify_fd(), [&] {
        queue.drain_notify();
        while (auto ev = queue.try_pop()) {
            switch (ev->type) {
                case BmsEventType::Connected:
                    spdlog::info("dispatcher: BMS connected");
                    state.set_connected(true);
                    jk_assembler.reset();
                    send_jk_request(kJkCmdDeviceInfo);
                    send_jk_request(kJkCmdCellInfo);
                    break;
                case BmsEventType::Disconnected:
                    spdlog::info("dispatcher: BMS disconnected");
                    state.set_connected(false);
                    jk_assembler.reset();
                    break;
                case BmsEventType::NotifyFrame: {
                    spdlog::trace("dispatcher: notify {} bytes: {:Xpn}",
                                  ev->payload.size(),
                                  spdlog::to_hex(ev->payload));
                    jk_assembler.push_chunk(ev->payload.data(), ev->payload.size());
                    while (auto frame = jk_assembler.take_complete_frame()) {
                        const auto type = jk_response_frame_type(frame->data(), frame->size());
                        if (!type) continue;

                        if (*type == kJkFrameTypeCellInfo) {
                            JkCellInfo cells;
                            JkPackInfo pack;
                            if (parse_cell_info(frame->data(), frame->size(), cells, pack)) {
                                spdlog::debug("BMS: V={:.3f} I={:.3f} SoC={}% cells={} cycles={}",
                                             pack.voltage_mv  / 1000.0,
                                             pack.current_ma  / 1000.0,
                                             pack.state_of_charge_pct,
                                             cells.voltages_mv.size(),
                                             pack.cycle_count);
                                state.apply_telemetry(std::move(cells), std::move(pack));
                            }
                        } else if (*type == kJkFrameTypeDeviceInfo) {
                            JkDeviceInfo d;
                            if (parse_device_info(frame->data(), frame->size(), d)) {
                                spdlog::debug("BMS device: vendor='{}' fw={} hw={} sn={}",
                                             d.vendor_id, d.firmware_version,
                                             d.hardware_version, d.serial_number);
                            }
                        } else if (*type == kJkFrameTypeSettings) {
                            JkSettings s;
                            if (parse_settings(frame->data(), frame->size(), s)) {
                                spdlog::debug("BMS settings: cells={} cell_ovp={}mV cell_uvp={}mV "
                                              "Imax_chg={}mA Imax_dis={}mA",
                                              s.cell_count, s.cell_ovp_mv, s.cell_uvp_mv,
                                              s.max_charge_current_ma, s.max_discharge_current_ma);
                                state.apply_settings(std::move(s));
                            }
                        } else {
                            spdlog::warn("dispatcher: ignoring unknown JK frame type {:#04x}", *type);
                        }
                    }
                    break;
                }
            }
        }
    });

    // Periodic poll for fresh cell_info. JK firmware appears to push frames
    // unsolicited after the first request, but a steady ping is cheap and
    // covers the case where it doesn't.
    dispatcher.add_timer(cfg.bms_poll_period_ms, [&] {
        spdlog::trace("dispatcher: poll tick — request cell_info");
        send_jk_request(kJkCmdCellInfo);
    });

    // Re-fetch device_info + settings on a slow cadence so changes the
    // operator makes in the JK app (e.g. tweaking max charge current) flow
    // through to our cached JkSettings without waiting for a BLE reconnect.
    dispatcher.add_timer(cfg.settings_refresh_period_ms, [&] {
        spdlog::trace("dispatcher: settings refresh tick — request device_info");
        send_jk_request(kJkCmdDeviceInfo);
    });

    ble.start();
    inv.start();

    dispatcher.run();

    spdlog::info("bms_bridge stopping");
    ble.stop();
    inv.stop();
    return EXIT_SUCCESS;
}
