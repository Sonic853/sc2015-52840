#include "xinput.h"
#include "xinput_descriptors.h"
#include "usb_tx_guard.h"
#include "radio.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <device/usbd_pvt.h>
#include <string.h>

namespace xinput {
// PC wired XInput. Accessory interfaces remain empty; no console authentication.
static volatile bool enabled;
static volatile bool opened;
static volatile uint32_t generation;
static volatile bool out_rearm;
static rumble::Mailbox rumble_mailbox;
static constexpr uint8_t ep_in = 0x81, ep_out = 2;
alignas(4) static uint8_t tx[20], rx[32];
alignas(4) static uint8_t latest[20] = {0,20}, control_input[20];
// Boot-lifetime counters, retained across reset and personality changes.
static struct {
	uint32_t opens, resets, os_queries, in_submitted, in_completed, in_failed;
	uint32_t out_completed, out_failed, controls, rejected;
	uint8_t last_setup[8];
} stats;
class Critical {
	uint32_t previous;
public:
	Critical() : previous(__get_PRIMASK()) { __disable_irq(); }
	~Critical() { if (!previous) __enable_irq(); }
};
static void put32(uint8_t *out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8 * i));
}
static void observeControl(uint8_t stage, const tusb_control_request_t *r, bool accepted)
{
	if (stage != CONTROL_STAGE_SETUP) return;
	Critical critical;
	++stats.controls;
	if (!accepted) ++stats.rejected;
	memcpy(stats.last_setup, r, sizeof(stats.last_setup));
}
void diagnostics(uint8_t out[64])
{
	// Only extend the reserved FD tail; the v1 mode header stays compatible.
	Critical critical;
	out[9] = 1;
	out[10] = uint8_t((enabled ? 1 : 0) | (opened ? 2 : 0) |
		(TinyUSBDevice.mounted() ? 4 : 0) | (TinyUSBDevice.suspended() ? 8 : 0));
	out[11] = 0;
	put32(out + 12, generation); put32(out + 16, stats.opens);
	put32(out + 20, stats.resets); put32(out + 24, stats.os_queries);
	put32(out + 28, stats.in_submitted); put32(out + 32, stats.in_completed);
	put32(out + 36, stats.in_failed); put32(out + 40, stats.out_completed);
	put32(out + 44, stats.out_failed); put32(out + 48, stats.controls);
	memcpy(out + 52, stats.last_setup, 8); put32(out + 60, stats.rejected);
}

class Interface : public Adafruit_USBD_Interface {
public:
	uint16_t getInterfaceDescriptor(uint8_t, uint8_t *out, uint16_t capacity) override
	{
		if (!out) return sizeof(descriptor);
		if (capacity < sizeof(descriptor)) return 0;
		uint8_t number = TinyUSBDevice.allocInterface(4);
		if (number != 0) return 0;
		for (unsigned ep = 1; ep <= 6; ++ep) {
			TinyUSBDevice.allocEndpoint(1); TinyUSBDevice.allocEndpoint(0);
		}
		memcpy(out, descriptor, sizeof(descriptor));
		return sizeof(descriptor);
	}
};
static Interface interface;
static const uint16_t os_string[] = {0x0312, 'M','S','F','T','1','0','0',0x20};
static const uint8_t os_compatible[40] = {
	40,0,0,0, 0,1, 4,0, 1,0,0,0,0,0,0,0,
	0,1, 'X','U','S','B','1','0',0,0,
	0,0,0,0,0,0,0,0, 0,0,0,0,0,0,
};
const uint16_t *osString() { return enabled ? os_string : nullptr; }
const uint16_t *securityString()
{
	static uint16_t text[sizeof(security_text)];
	text[0] = uint16_t(0x0300 | sizeof(text));
	for (unsigned i = 0; i < sizeof(security_text) - 1; ++i) text[i + 1] = uint8_t(security_text[i]);
	return enabled ? text : nullptr;
}
bool vendorControl(uint8_t port, uint8_t stage, const tusb_control_request_t *r)
{
	if (!enabled || port) return false;
	bool accepted = r->bmRequestType == 0xc0 && r->bRequest == 0x20 && !r->wValue && r->wIndex == 4 && r->wLength;
	observeControl(stage, r, accepted);
	if (!accepted) return false;
	if (stage == CONTROL_STAGE_SETUP) { Critical critical; ++stats.os_queries; }
	return stage != CONTROL_STAGE_SETUP || tud_control_xfer(port, r, const_cast<uint8_t *>(os_compatible), sizeof(os_compatible));
}
bool addInterface() { return TinyUSBDevice.addInterface(interface); }
void stopRumble()
{
	Critical critical;
	rumble_mailbox.stop();
}
uint32_t setRumbleTarget(int slot, uint32_t session)
{
	Critical critical;
	return rumble_mailbox.target(slot, session, enabled && opened && TinyUSBDevice.mounted() && !TinyUSBDevice.suspended());
}
bool takeRumble(rumble::Command &command)
{
	Critical critical;
	return rumble_mailbox.take(command);
}
void enable(bool value)
{
	Critical critical;
	enabled = value; opened = false; out_rearm = false; ++generation;
	stopRumble();
	memset(latest, 0, sizeof(latest)); latest[1] = sizeof(latest);
}
static void reset(uint8_t)
{
	Critical critical;
	opened = false; out_rearm = false; ++generation; ++stats.resets;
	stopRumble();
	memset(latest, 0, sizeof(latest)); latest[1] = sizeof(latest);
}
static void init() { reset(0); }
static bool deinit() { reset(0); return true; }
static uint16_t open(uint8_t port, const tusb_desc_interface_t *itf, uint16_t length)
{
	if (!enabled || port || !itf || length < 9 || itf->bInterfaceNumber >= 4) return 0;
	static const uint16_t starts[] = {0,40,104,129,144};
	unsigned number = itf->bInterfaceNumber;
	uint16_t size = starts[number + 1] - starts[number];
	if (length < size || memcmp(itf, descriptor + starts[number], size)) return 0;
	// Claim each accessory interface but do not open its unused endpoints.
	if (number) return size;
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(itf);
	if (!usbd_edpt_open(port, reinterpret_cast<const tusb_desc_endpoint_t *>(bytes + 26))) return 0;
	if (!usbd_edpt_open(port, reinterpret_cast<const tusb_desc_endpoint_t *>(bytes + 33))) {
		usbd_edpt_close(port, 0x81); return 0;
	}
	{ Critical critical; ++generation; opened = true; ++stats.opens; }
	out_rearm = !usbd_edpt_xfer(port, ep_out, rx, sizeof(rx));
	if (out_rearm) { Critical critical; ++stats.out_failed; }
	return size;
}
static bool control(uint8_t port, uint8_t stage, const tusb_control_request_t *r)
{
	if (!enabled || !opened || port) return false;
	// MS-XUSBI table 30, gamepad input report zero on interface zero.
	bool accepted = r->bmRequestType == 0xa1 && r->bRequest == 1 &&
		r->wValue == 0x0100 && r->wIndex == 0 && r->wLength;
	observeControl(stage, r, accepted);
	if (!accepted) return false;
	if (stage != CONTROL_STAGE_SETUP) return true;
	{ Critical critical; memcpy(control_input, latest, sizeof(control_input)); }
	// A separate EP0 snapshot cannot be overwritten by the interrupt IN pump.
	return tud_control_xfer(port, r, control_input, sizeof(control_input));
}
static bool transfer(uint8_t port, uint8_t endpoint, xfer_result_t result, uint32_t count)
{
	if (!opened || port) return false;
	if (endpoint == ep_out) {
		// Parse only the wired Xbox rumble report; never pass arbitrary OUT
		// bytes through as Steam Feature commands. Latest motor state wins.
		{ Critical critical;
			if (result == XFER_RESULT_SUCCESS && count <= sizeof(rx)) ++stats.out_completed;
			else ++stats.out_failed;
			if (result == XFER_RESULT_SUCCESS && count == 8 && rx[0] == 0 && rx[1] == 8 &&
			    rx[2] == 0 && rx[5] == 0 && rx[6] == 0 && rx[7] == 0 &&
			    enabled && TinyUSBDevice.mounted() && !TinyUSBDevice.suspended()) {
				rumble_mailbox.submit(rx[3], rx[4], radio::queueMicros());
			}
		}
		out_rearm = !usbd_edpt_xfer(port, ep_out, rx, sizeof(rx));
		if (out_rearm) { Critical critical; ++stats.out_failed; }
		return true;
	}
	if (endpoint != ep_in) return false;
	Critical critical;
	if (result == XFER_RESULT_SUCCESS && count == sizeof(tx)) ++stats.in_completed;
	else ++stats.in_failed;
	return true;
}
static const usbd_class_driver_t driver = {"XINPUT", init, deinit, reset, open, control, transfer, nullptr};
void service()
{
	usb::TxGuard guard;
	uint32_t epoch = generation;
	if (!enabled || !opened || !out_rearm || !TinyUSBDevice.mounted() ||
	    TinyUSBDevice.suspended() || !usbd_edpt_claim(0, ep_out)) return;
	if (!opened || generation != epoch) { usbd_edpt_release(0, ep_out); return; }
	out_rearm = false;
	if (!usbd_edpt_xfer(0, ep_out, rx, sizeof(rx))) {
		if (generation == epoch) { Critical critical; out_rearm = true; ++stats.out_failed; }
		usbd_edpt_release(0, ep_out);
	}
}
bool send(const uint8_t report[20])
{
	usb::TxGuard guard;
	uint32_t epoch;
	{
		Critical critical;
		if (!enabled || !opened) return false;
		epoch = generation;
		memcpy(latest, report, sizeof(latest));
	}
	if (!opened || !enabled || !TinyUSBDevice.mounted() || TinyUSBDevice.suspended() ||
	    !usbd_edpt_claim(0, ep_in)) return false;
	if (!opened || generation != epoch) { usbd_edpt_release(0, ep_in); return false; }
	memcpy(tx, report, sizeof(tx));
	if (usbd_edpt_xfer(0, ep_in, tx, sizeof(tx))) {
		Critical critical; ++stats.in_submitted; return true;
	}
	{ Critical critical; ++stats.in_failed; }
	usbd_edpt_release(0, ep_in);
	return false;
}
}

// A linker wrapper keeps the callback strong despite TinyUSB's weak declaration.
extern "C" const usbd_class_driver_t *__wrap_usbd_app_driver_get_cb(uint8_t *count)
{
	*count = 1;
	return &xinput::driver;
}
