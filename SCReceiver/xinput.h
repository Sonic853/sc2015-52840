#pragma once
#include <stdint.h>
#include <Adafruit_TinyUSB.h>
namespace xinput {
bool addInterface();
void enable(bool enabled);
bool send(const uint8_t report[20]);
const uint16_t *osString();
bool vendorControl(uint8_t port, uint8_t stage, const tusb_control_request_t *request);
}
