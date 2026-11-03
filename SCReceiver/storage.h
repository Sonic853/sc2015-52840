#pragma once

#include "protocol.h"
#include "input_mode.h"

namespace storage {

constexpr uint32_t PAGE_A = 0xeb000;
constexpr uint32_t PAGE_B = 0xec000;
constexpr uint32_t CONFIG_A = 0xe9000;
constexpr uint32_t CONFIG_B = 0xea000;
bool loadInputConfig(input_mode::Config &config, input_mode::Mode &boot_mode);
bool saveInputConfig(const input_mode::Config &config);
bool saveBootMode(input_mode::Mode mode);
bool load(sc::Database &database);
bool save(const sc::Database &database);
void radioRunning(bool running);

} // namespace storage
