#pragma once

#include <string>

namespace kopidb {

void init_spdlog_file();
void reset_log_level(const std::string &level);

} // namespace kopidb