#include "ble_client.hpp"
#include "bms_handler.hpp"
#include "config.hpp"
#include "dispatcher.hpp"
#include "event_queue.hpp"
#include "history.hpp"
#include "http_server.hpp"
#include "inverter.hpp"
#include "logger.hpp"
#include "shared_state.hpp"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-D level] [-U device_uuid] [-d db_path]\n"
              << "  -Dx   log verbosity 1..5 (1=error, 2=warning, 3=info, 4=debug, 5=trace)\n"
              << "  -Uxxx BLE address/identifier of the device to connect to\n"
              << "  -dxxx path to the telemetry history SQLite file\n";
}

constexpr const char* kDefaultConfigPath = "bms_bridge.conf";

}  // namespace

int main(int argc, char** argv) {
    std::string cli_uuid;
    std::string cli_db_path;
    int cli_log_level = -1;

    int opt;
    while ((opt = getopt(argc, argv, "D:U:d:h")) != -1) {
        switch (opt) {
            case 'D':
                cli_log_level = std::atoi(optarg);
                if (cli_log_level < 1 || cli_log_level > 5) {
                    std::cerr << "log level must be in [1..5]\n";
                    return EXIT_FAILURE;
                }
                break;
            case 'U': cli_uuid    = optarg; break;
            case 'd': cli_db_path = optarg; break;
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

    if (cli_log_level > 0)    cfg.log_level       = cli_log_level;
    if (!cli_uuid.empty())    cfg.device_uuid     = cli_uuid;
    if (!cli_db_path.empty()) cfg.history_db_path = cli_db_path;

    init_logger(cfg.log_level, cfg.log_file);

    if (cfg.device_uuid.empty() || cfg.device_uuid == "00:00:00:00:00:00") {
        spdlog::critical("device uuid is not configured (use -U or set device_uuid in config)");
        return EXIT_FAILURE;
    }

    spdlog::info("bms_bridge starting; bms={} uart={}@{}",
                 cfg.device_uuid, cfg.uart_device, cfg.uart_baud);

    SharedState    state;
    BmsEventQueue  queue;
    Dispatcher     dispatcher;
    History        history(cfg.history_db_path,
                           std::chrono::seconds(cfg.history_ram_window_s));
    HttpServer     http_server(history, state,
                               static_cast<uint16_t>(cfg.http_port),
                               cfg.www_root);

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
    Inverter  inv(cfg.uart_device, cfg.uart_baud, state);

    BmsHandler bms(ble, queue, state, history,
                   std::chrono::milliseconds(cfg.bms_poll_period_ms),
                   std::chrono::milliseconds(cfg.bms_state_update_interval_ms));

    dispatcher.watch(queue.notify_fd(),
                     [&bms] { bms.on_queue_event(); });
    dispatcher.add_timer(cfg.bms_poll_period_ms,
                         [&bms] { bms.on_poll_tick(); });
    dispatcher.add_timer(cfg.settings_refresh_period_ms,
                         [&bms] { bms.on_settings_tick(); });

    inv.attach(dispatcher);
    ble.start();
    http_server.start();

    dispatcher.run();

    spdlog::info("bms_bridge stopping");
    http_server.stop();
    ble.stop();
    return EXIT_SUCCESS;
}
