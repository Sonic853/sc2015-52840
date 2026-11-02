#pragma once

#include "protocol.h"

namespace storage {

constexpr uint32_t PAGE_A = 0xeb000;
constexpr uint32_t PAGE_B = 0xec000;
bool load(sc::Database &database);
bool save(const sc::Database &database);
void radioRunning(bool running);

} // namespace storage
