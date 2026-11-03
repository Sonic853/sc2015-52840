#pragma once

#include "protocol.h"
#include "queue_policy.h"

namespace usb {

bool begin();
unsigned process(sc::Receiver &receiver, unsigned budget = sc::queue_policy::USB_BATCH,
		 uint32_t haptic_wait_us = sc::queue_policy::HAPTIC_WAIT_US);
void observeRxLatency(uint32_t age_us);
void publish(const sc::Receiver &receiver);
void pump();
void input(unsigned slot, const uint8_t report[64]);
void desktop(const uint8_t keyboard[8], const uint8_t mouse[4]);
// Loop-task only, alongside desktop()/pump(). Disable clears queued desktop
// state; false then the current enable value also resets it after USB reset.
void desktopEnabled(bool enable);

} // namespace usb
