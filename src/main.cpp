#include "battery.hpp"
#include "config.hpp"
#include "dispatcher.hpp"
#include "history.hpp"
#include "http_server.hpp"
#include "inverter.hpp"
#include "logger.hpp"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [-D level] [-d db_path]\n"
              << "  -Dx   log verbosity 1..5 (1=error 2=warning 3=info "
                 "4=debug 5=trace)\n"
              << "  -dxxx override the telemetry history SQLite path\n";
}

constexpr const char* kDefaultConfigPath = "bms_bridge.conf";

}  // namespace

int main(int argc, char** argv) {
    std::string cli_db_path;
    int cli_log_level = -1;

    int opt;
    while ((opt = getopt(argc, argv, "D:d:h")) != -1) {
        switch (opt) {
            case 'D':
                cli_log_level = std::atoi(optarg);
                if (cli_log_level < 1 || cli_log_level > 5) {
                    std::cerr << "log level must be in [1..5]\n";
                    return EXIT_FAILURE;
                }
                break;
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
    if (!cli_db_path.empty()) cfg.history.db_path = cli_db_path;

    init_logger(cfg.log_level, cfg.log_file);

    spdlog::info("bms_bridge starting; {} batteries, inverter uart={}@{}",
                 cfg.batteries.size(), cfg.inverter.uart_device,
                 cfg.inverter.uart_baud);

    History    history(cfg.history.db_path,
                       std::chrono::seconds(cfg.history.ram_window_s));
    Dispatcher dispatcher;

    std::vector<std::unique_ptr<Battery>> batteries;
    batteries.reserve(cfg.batteries.size());
    for (auto& bc : cfg.batteries)
        batteries.push_back(std::make_unique<Battery>(bc));

    // Every configured battery physically exists, so it must have a row in
    // the batteries table (hence an id, hence appear in /batteries) even
    // before its first sample — a reserve battery never appends at all.
    for (const auto& bc : cfg.batteries)
        history.ensure_battery_id(bc.name);

    // Migrate / catch up on all days older than the retention window
    history.aggregate_pending();

    // Re-check every hour in case the process was running at midnight
    dispatcher.add_timer(3600000, [&history] {
        history.aggregate_pending();
    });

    for (auto& b : batteries)
        b->start(dispatcher, history);

    Inverter inv(cfg.inverter.uart_device, cfg.inverter.uart_baud, batteries);
    inv.attach(dispatcher);

    HttpServer http_server(history, batteries,
                           cfg.http.http_port, cfg.http.www_root);
    http_server.start();

    dispatcher.run();

    spdlog::info("bms_bridge stopping");
    http_server.stop();
    for (auto& b : batteries)
        b->stop();
    return EXIT_SUCCESS;
}
