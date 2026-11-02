#include "usb.h"
#include "radio.h"
#include <Adafruit_TinyUSB.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define REQUIRE(value) do { if (!(value)) { \
	std::fprintf(stderr, "USB FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

namespace radio {
const Statistics &statistics() { static Statistics stats{}; return stats; }
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
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64 && reply[0] == 0xad);
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
	REQUIRE(reply[1] == 62 && reply[2] == 2 && reply[33] == 0 && reply[34] == 4);
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
