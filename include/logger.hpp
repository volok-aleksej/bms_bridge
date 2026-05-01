#pragma once

#include <spdlog/spdlog.h>

#include <string>

void init_logger(int verbosity, const std::string& log_file = "");
