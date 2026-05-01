#include "logger.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <vector>

void init_logger(int verbosity, const std::string& log_file) {
    if (verbosity < 1) verbosity = 1;
    if (verbosity > 5) verbosity = 5;

    spdlog::level::level_enum lvl = spdlog::level::info;
    switch (verbosity) {
        case 1: lvl = spdlog::level::err;   break;
        case 2: lvl = spdlog::level::warn;  break;
        case 3: lvl = spdlog::level::info;  break;
        case 4: lvl = spdlog::level::debug; break;
        case 5: lvl = spdlog::level::trace; break;
    }

    std::vector<spdlog::sink_ptr> sinks;

    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    stdout_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    sinks.push_back(stdout_sink);

    if (!log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, false);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("bms", sinks.begin(), sinks.end());
    logger->set_level(lvl);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}
