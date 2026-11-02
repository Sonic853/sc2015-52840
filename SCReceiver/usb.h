#pragma once

#include "protocol.h"

namespace usb {

bool begin();
void process(sc::Receiver &receiver);
void publish(const sc::Receiver &receiver);
void pump();
void input(unsigned slot, const uint8_t report[64]);
void desktop(const uint8_t keyboard[8], const uint8_t mouse[4]);

} // namespace usb
