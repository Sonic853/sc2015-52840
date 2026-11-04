#pragma once
#include <Arduino.h>
#include <nrf_gpio.h>

namespace board_profile {
inline void begin()
{
#if defined(SC2015_BOARD_PROMICRO)
	// OpenPuck's SuperMini/ProMicro LED is P0.15, not the borrowed
	// Feather variant's LED_BUILTIN. Do not touch XIAO QSPI pins here.
	nrf_gpio_pin_clear(15);
	nrf_gpio_cfg_output(15);
#else
	pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT); pinMode(LED_BLUE, OUTPUT);
	digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_BLUE, HIGH);
#endif
	nrf_gpio_cfg_input(2, NRF_GPIO_PIN_PULLUP);
}
inline bool pairingPressed() { return nrf_gpio_pin_read(2) == 0; }
inline void fatal(bool on)
{
#if defined(SC2015_BOARD_PROMICRO)
	nrf_gpio_pin_write(15, on ? 1 : 0);
#else
	digitalWrite(LED_RED, on ? LOW : HIGH);
#endif
}
inline void status(bool pairing, bool connected, uint32_t now_ms)
{
#if defined(SC2015_BOARD_PROMICRO)
	nrf_gpio_pin_write(15, pairing ? now_ms % 500 < 250 : connected);
#else
	digitalWrite(LED_BLUE, pairing && now_ms % 500 < 250 ? LOW : HIGH);
	digitalWrite(LED_GREEN, connected ? LOW : HIGH);
#endif
}
}
