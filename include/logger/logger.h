#pragma once

#include <string>

namespace tiny_lsm {

void init_spdlog_file();
void reset_log_level(const std::string &level);

} // namespace tiny_lsm