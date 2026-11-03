#pragma once
#include <Adafruit_TinyUSB.h>
#include "rumble.h"

namespace ps5 {
const uint8_t *reportDescriptor();
uint16_t reportDescriptorSize();
void enable(bool enabled, uint32_t id0, uint32_t id1);
void reset();
// Input is the existing mapped gamepad payload, keeping remaps/deadzones shared.
void buildReport(const uint8_t mapped[20], uint32_t now_us, uint8_t out[63]);
uint16_t getReport(uint8_t id, hid_report_type_t type, uint8_t *out, uint16_t capacity);
void setReport(uint8_t id, hid_report_type_t type, const uint8_t *data, uint16_t length);
uint32_t setRumbleTarget(int slot, uint32_t session);
bool takeRumble(rumble::Command &command);
void stopRumble();
}
