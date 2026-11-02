#include "usb.h"
#include "radio.h"
#include "radio_policy.h"
#include <Adafruit_TinyUSB.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define REQUIRE(value) do { if (!(value)) { \
	std::fprintf(stderr, "USB FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

namespace radio {
const Statistics &statistics() { static Statistics stats{}; return stats; }
void pairingDiagnostics(uint8_t report[64])
{
	std::memset(report, 0, 64);
	report[0] = 0xf7; report[1] = 62; report[2] = 1;
	report[3] = report[63] = FIRMWARE_REVISION;
}
}

extern "C" const uint8_t *__real_tud_descriptor_device_cb()
{
	static tusb_desc_device_t device{};
	device.bDeviceClass = 0xef;
	device.iSerialNumber = 3;
	return reinterpret_cast<const uint8_t *>(&device);
}
extern "C" const uint8_t *__wrap_tud_descriptor_device_cb();

static bool unusedAes(const uint8_t[16], const uint8_t[16], uint8_t[16]) { return false; }
static bool randomBytes(uint8_t *data, size_t size) { std::memset(data, 0x43, size); return true; }
static bool persist(const sc::Database &) { return true; }

// Exercise production control-transfer wrappers with the pinned TinyUSB order:
// SETUP -> tud_control_xfer(buffer) -> OUT DATA -> optional status ACK callback.
// The framework shim never executes protocol code; tests drive usb::process.
static tusb_control_request_t control_request;
static uint8_t control_buffer[64];
extern "C" bool __wrap_tud_control_xfer(uint8_t, const tusb_control_request_t *, void *, uint16_t);
extern "C" bool __wrap_hidd_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
extern "C" bool __wrap_usbd_control_xfer_cb(uint8_t, uint8_t, xfer_result_t, uint32_t);
extern "C" bool __real_tud_control_xfer(uint8_t, const tusb_control_request_t *, void *, uint16_t) { return true; }
extern "C" bool __real_hidd_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
	if (stage == CONTROL_STAGE_SETUP)
		return request->wLength <= 64 && __wrap_tud_control_xfer(rhport, request, control_buffer, request->wLength);
	if (stage == CONTROL_STAGE_ACK)
		Adafruit_USBD_HID::instances[request->wIndex]->set(0, HID_REPORT_TYPE_FEATURE, control_buffer, request->wLength);
	return true;
}
extern "C" bool __real_usbd_control_xfer_cb(uint8_t rhport, uint8_t, xfer_result_t, uint32_t)
{
	return __wrap_hidd_control_xfer_cb(rhport, CONTROL_STAGE_DATA, &control_request);
}
bool mockFeatureSet(unsigned interface_number, const uint8_t *data, unsigned requested,
		    unsigned actual, xfer_result_t result)
{
	control_request = {0x21, 9, 0x0300, uint16_t(interface_number), uint16_t(requested)};
	if (!__wrap_hidd_control_xfer_cb(0, CONTROL_STAGE_SETUP, &control_request))
		return false;
	REQUIRE(actual <= 64);
	if (requested) {
		std::memcpy(control_buffer, data, actual);
		if (!__wrap_usbd_control_xfer_cb(0, 0, result, actual))
			return false;
	}
	dcd_edpt0_status_complete(0, &control_request);
	return __wrap_hidd_control_xfer_cb(0, CONTROL_STAGE_ACK, &control_request);
}

void testUsb()
{
	sc::Database db{};
	db.address[1] = 0x42;
	sc::Receiver receiver;
	sc::Hooks hooks = {unusedAes, randomBytes, persist, usb::input, usb::desktop};
	REQUIRE(receiver.begin(db, hooks));
	REQUIRE(usb::begin());
	REQUIRE(TinyUSBDevice.vid == 0x28de && TinyUSBDevice.pid == 0x1142);
	REQUIRE(Adafruit_USBD_HID::instances.size() == 5);
	for (unsigned i = 0; i < 5; ++i) {
		auto *hid = Adafruit_USBD_HID::instances[i];
		uint8_t descriptor[25];
		REQUIRE(hid->getInterfaceDescriptor(uint8_t(i), descriptor, 25) == 25);
		REQUIRE(descriptor[11] == 0x11 && descriptor[12] == 1);
		REQUIRE(descriptor[22] == (i ? 64 : 9));
		REQUIRE(hid->report_size == (i ? 33u : 121u));
	}
	const auto *device = reinterpret_cast<const tusb_desc_device_t *>(__wrap_tud_descriptor_device_cb());
	REQUIRE(device->bDeviceClass == 0 && device->iSerialNumber == 0);
	auto *slot = Adafruit_USBD_HID::instances[1];
	uint8_t command[64] = {0xad, 2, 1, 0}, reply[64];
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	REQUIRE(!receiver.pairing());
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
	usb::process(receiver);
	REQUIRE(receiver.pairing());
	command[0] = 0xb4; command[1] = 0;
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	REQUIRE(reply[0] == 0xb4 && reply[1] == 1 && reply[2] == 1);
	usb::process(receiver);
	REQUIRE(slot->get(1, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_INPUT, reply, 64) == 0);
	for (unsigned i = 0; i < 40; ++i)
		slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	for (unsigned i = 0; i < 4; ++i)
		usb::process(receiver);
	command[0] = 0xf0;
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	REQUIRE(reply[0] == 0xf0 && reply[55] >= 9);
	REQUIRE(reply[1] == 62 && reply[2] == 3 && reply[33] == 0 && reply[34] == 4);
	REQUIRE(!std::memcmp(reply + 59, db.address, 5));
	usb::process(receiver);
	for (unsigned i = 1; i < 4; ++i) {
		auto *other = Adafruit_USBD_HID::instances[i + 1];
		other->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
		usb::process(receiver);
		usb::publish(receiver);
		REQUIRE(other->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
		REQUIRE(reply[33] == i && reply[34] == 4);
		REQUIRE(!std::memcmp(reply + 59, db.address, 5));
		usb::process(receiver);
	}
	command[0] = 0xf7;
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	REQUIRE(reply[0] == 0xf7 && reply[2] == 1 && reply[3] == radio::FIRMWARE_REVISION && reply[63] == radio::FIRMWARE_REVISION);
	usb::process(receiver);
	command[0] = 0xf8;
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	REQUIRE(reply[0] == 0xf8 && reply[1] == 62 && reply[2] == 1 && reply[3] == radio::FIRMWARE_REVISION && reply[63] == radio::FIRMWARE_REVISION);
	usb::process(receiver);
	// A reset needs the magic, a full published reply, and deferred consumption.
	command[0] = 0xf9; command[1] = 4;
	std::memcpy(command + 2, "UF2!", 4);
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
	usb::process(receiver);
	REQUIRE(!receiver.takeBootloaderRequest());
	usb::publish(receiver);
	// Aborted GET data/status cannot trigger reboot, even though SETUP ran.
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64, false) == 64);
	usb::process(receiver);
	REQUIRE(!receiver.takeBootloaderRequest());
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 3) == 3);
	usb::process(receiver);
	REQUIRE(!receiver.takeBootloaderRequest());
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	REQUIRE(reply[0] == 0xf9 && reply[1] == 2 && reply[2] == 0 && reply[3] == 1);
	REQUIRE(!receiver.takeBootloaderRequest());
	usb::process(receiver);
	REQUIRE(receiver.takeBootloaderRequest() && !receiver.takeBootloaderRequest());
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	usb::process(receiver);
	REQUIRE(!receiver.takeBootloaderRequest());
	command[2] = 0;
	slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64 && reply[2] == 1);
	usb::process(receiver);
	REQUIRE(!receiver.takeBootloaderRequest());
	uint8_t input[64] = {1, 0, 1, 60};
	for (unsigned i = 0; i < 40; ++i) {
		input[8] = uint8_t(i);
		usb::input(0, input);
	}
	for (unsigned i = 0; i < 10; ++i)
		usb::pump();
	REQUIRE(!slot->sent.empty());
	REQUIRE(slot->sent.back().size() == 64 && slot->sent.back()[8] == 39);
	TinyUSBDevice.suspended_state = true;
	size_t before = slot->sent.size();
	usb::input(0, input);
	usb::pump();
	REQUIRE(slot->sent.size() == before);
	TinyUSBDevice.mounted_state = false;
	usb::pump();
	TinyUSBDevice.mounted_state = true;
	TinyUSBDevice.suspended_state = false;
	usb::pump();
	REQUIRE(slot->sent.size() == before);
	std::puts("PASS: production USB bridge with mock TinyUSB: descriptors, deferred callbacks, queue limits and suspend");
}
