#include "xinput.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <device/usbd_pvt.h>
#include <string.h>

namespace xinput {
// PC wired XInput interface. This does not implement console authentication,
// accessory interfaces or a proprietary-radio Xbox receiver.
static const uint8_t descriptor[40] = {
	9,4,0,0,2,0xff,0x5d,1,0,
	17,0x21,0,1,1,0x25,0x81,20,0,0,0,0x13,0x02,8,0,0,0,
	7,5,0x81,3,32,0,4,
	7,5,0x02,3,32,0,4,
};
static volatile bool enabled;
static volatile bool opened;
static volatile uint32_t generation;
static constexpr uint8_t ep_in = 0x81, ep_out = 2;
alignas(4) static uint8_t tx[20], rx[32];

class Interface : public Adafruit_USBD_Interface {
public:
	uint16_t getInterfaceDescriptor(uint8_t, uint8_t *out, uint16_t capacity) override
	{
		if (!out) return sizeof(descriptor);
		if (capacity < sizeof(descriptor)) return 0;
		uint8_t number = TinyUSBDevice.allocInterface(1);
		uint8_t in = TinyUSBDevice.allocEndpoint(1);
		TinyUSBDevice.allocEndpoint(0); // Wired XInput reserves OUT endpoint one.
		uint8_t out_ep = TinyUSBDevice.allocEndpoint(0);
		if (number != 0 || in != 0x81 || out_ep != 2) return 0;
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
bool vendorControl(uint8_t port, uint8_t stage, const tusb_control_request_t *r)
{
	if (!enabled || port || r->bmRequestType != 0xc0 || r->bRequest != 0x20 || r->wValue || r->wIndex != 4) return false;
	return stage != CONTROL_STAGE_SETUP || tud_control_xfer(port, r, const_cast<uint8_t *>(os_compatible), sizeof(os_compatible));
}
bool addInterface() { return TinyUSBDevice.addInterface(interface); }
void enable(bool value) { enabled = value; opened = false; ++generation; }
static void reset(uint8_t) { opened = false; ++generation; }
static void init() { reset(0); }
static bool deinit() { reset(0); return true; }
static uint16_t open(uint8_t port, const tusb_desc_interface_t *itf, uint16_t length)
{
	if (!enabled || port || length < sizeof(descriptor) || memcmp(itf, descriptor, sizeof(descriptor))) return 0;
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(itf);
	if (!usbd_edpt_open(port, reinterpret_cast<const tusb_desc_endpoint_t *>(bytes + 26))) return 0;
	if (!usbd_edpt_open(port, reinterpret_cast<const tusb_desc_endpoint_t *>(bytes + 33))) {
		usbd_edpt_close(port, 0x81); return 0;
	}
	++generation; opened = true;
	usbd_edpt_xfer(port, ep_out, rx, sizeof(rx));
	return sizeof(descriptor);
}
static bool control(uint8_t, uint8_t, const tusb_control_request_t *) { return false; }
static bool transfer(uint8_t port, uint8_t endpoint, xfer_result_t, uint32_t)
{
	if (!opened || port) return false;
	if (endpoint == ep_out) {
		// Drain LED/rumble writes; this revision implements input/configuration.
		// Never reinterpret Xbox OUT bytes as SC2015 Feature commands.
		usbd_edpt_xfer(port, ep_out, rx, sizeof(rx));
		return true;
	}
	return endpoint == ep_in;
}
static const usbd_class_driver_t driver = {"XINPUT", init, deinit, reset, open, control, transfer, nullptr};
bool send(const uint8_t report[20])
{
	uint32_t epoch = generation;
	if (!opened || !enabled || !TinyUSBDevice.mounted() || TinyUSBDevice.suspended() ||
	    !usbd_edpt_claim(0, ep_in)) return false;
	if (!opened || generation != epoch) { usbd_edpt_release(0, ep_in); return false; }
	memcpy(tx, report, sizeof(tx));
	if (usbd_edpt_xfer(0, ep_in, tx, sizeof(tx))) return true;
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
