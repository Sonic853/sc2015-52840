#pragma once
#include <Adafruit_TinyUSB.h>
#include "rumble.h"
#include "motion.h"

namespace switch_pro {
struct Report {
	uint8_t id, data[63];
	uint32_t epoch, serial;
};
const uint8_t *reportDescriptor();
uint16_t reportDescriptorSize();
void enable(bool enabled, uint32_t id0, uint32_t id1);
void reset();
// Snapshot, then commit only after successful USB submission; resets invalidate tickets.
bool nextReport(const uint8_t mapped[20], uint32_t now, Report &report, const motion::Sample *sample = nullptr);
bool current(const Report &report);
void sent(const Report &report, uint32_t now);
uint8_t minimumOutputSize(uint8_t id);
bool validOutput(uint8_t id, const uint8_t *data, uint16_t length);
uint16_t getReport(uint8_t id, hid_report_type_t type, uint8_t *out, uint16_t capacity);
void setReport(uint8_t id, hid_report_type_t type, const uint8_t *data, uint16_t length);
uint32_t setRumbleTarget(int slot, uint32_t session);
bool takeRumble(rumble::Command &command);
void stopRumble();
}
