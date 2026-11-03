#pragma once
#include <stdint.h>
#include <Adafruit_TinyUSB.h>
#include "rumble.h"
namespace xinput {
bool addInterface();
void enable(bool enabled);
bool send(const uint8_t report[20]);
// Called from the loop; retries a failed OUT receive without re-enumeration.
void service();
// Capture the RF destination at USB receipt, never when a later loop consumes it.
// Zero means output is currently disabled/unmounted/suspended.
uint32_t setRumbleTarget(int slot, uint32_t session);
bool takeRumble(rumble::Command &command);
void stopRumble();
void diagnostics(uint8_t report[64]);
const uint16_t *osString();
const uint16_t *securityString();
bool vendorControl(uint8_t port, uint8_t stage, const tusb_control_request_t *request);
}
