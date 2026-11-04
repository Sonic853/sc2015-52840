#include "usb.h"
#include "radio.h"
#include "radio_policy.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <device/dcd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define REQUIRE(value) do { if (!(value)) { \
	std::fprintf(stderr, "USB FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

namespace radio {
uint32_t queueMicros() { return micros(); }
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
extern "C" void tud_event_hook_cb(uint8_t, uint32_t, bool);
extern "C" const uint8_t *__wrap_tud_descriptor_bos_cb();
extern "C" bool __wrap_tud_vendor_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
extern "C" bool __real_tud_control_xfer(uint8_t, const tusb_control_request_t *r, void *p, uint16_t n)
{
	if (r->bmRequestType & 0x80) {
		mock_control_pointer = static_cast<const uint8_t *>(p);
		uint16_t count = n < r->wLength ? n : r->wLength;
		mock_control_input.clear();
		if (p && count) mock_control_input.assign(mock_control_pointer, mock_control_pointer + count);
	}
	return true;
}
extern "C" bool tud_control_xfer(uint8_t port, const tusb_control_request_t *r, void *p, uint16_t n) { return __wrap_tud_control_xfer(port, r, p, n); }
extern "C" const uint16_t *__real_tud_descriptor_string_cb(uint8_t, uint16_t) { return nullptr; }
extern "C" bool __real_hidd_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
	if (stage == CONTROL_STAGE_SETUP)
		return request->wLength <= 64 && __wrap_tud_control_xfer(rhport, request, control_buffer, request->wLength);
	if (stage == CONTROL_STAGE_ACK)
		for (auto *instance : Adafruit_USBD_HID::instances)
			if (instance->interface_number == request->wIndex && instance->set) {
				uint8_t id = uint8_t(request->wValue);
				const uint8_t *data = control_buffer; uint16_t length = request->wLength;
				if (id && length > 1 && data[0] == id) { ++data; --length; }
				instance->set(id, hid_report_type_t(request->wValue >> 8), data, length);
			}
	return true;
}
extern "C" bool __real_usbd_control_xfer_cb(uint8_t rhport, uint8_t, xfer_result_t, uint32_t)
{
	return __wrap_hidd_control_xfer_cb(rhport, CONTROL_STAGE_DATA, &control_request);
}
static bool mockReportData(unsigned interface_number, hid_report_type_t type,
			   const uint8_t *data, unsigned requested, unsigned actual,
			   xfer_result_t result = XFER_RESULT_SUCCESS, uint8_t id = 0)
{
	control_request = {0x21, 9, uint16_t((type << 8) | id), uint16_t(interface_number), uint16_t(requested)};
	if (!__wrap_hidd_control_xfer_cb(0, CONTROL_STAGE_SETUP, &control_request))
		return false;
	REQUIRE(actual <= 64);
	if (requested) {
		std::memcpy(control_buffer, data, actual);
		if (!__wrap_usbd_control_xfer_cb(0, 0, result, actual))
			return false;
	}
	return true;
}

static bool mockReportAck()
{
	dcd_edpt0_status_complete(0, &control_request);
	return __wrap_hidd_control_xfer_cb(0, CONTROL_STAGE_ACK, &control_request);
}

bool mockFeatureSet(unsigned interface_number, const uint8_t *data, unsigned requested,
		    unsigned actual, xfer_result_t result)
{
	return mockReportData(interface_number, HID_REPORT_TYPE_FEATURE, data, requested, actual, result) &&
	       mockReportAck();
}

bool mockFeatureData(unsigned interface_number, const uint8_t *data, unsigned requested, unsigned actual)
{
	return mockReportData(interface_number, HID_REPORT_TYPE_FEATURE, data, requested, actual);
}

bool mockFeatureAck() { return mockReportAck(); }
bool mockPs5Output(const uint8_t *data, unsigned requested, unsigned actual, xfer_result_t result, bool complete)
{
	return mockReportData(0, HID_REPORT_TYPE_OUTPUT, data, requested, actual, result, 2) && (!complete || mockReportAck());
}
bool mockHidOutput(uint8_t id, const uint8_t *data, unsigned requested, unsigned actual, xfer_result_t result, bool complete)
{
	return mockReportData(0, HID_REPORT_TYPE_OUTPUT, data, requested, actual, result, id) && (!complete || mockReportAck());
}

void testUsbControlEdges()
{
	sc::Receiver receiver;
	sc::Database db{};
	db.address[1] = 0x42;
	sc::Hooks hooks = {unusedAes, randomBytes, persist, usb::input, usb::desktop};
	REQUIRE(receiver.begin(db, hooks));
	auto *slot = Adafruit_USBD_HID::instances[1];
	auto *other = Adafruit_USBD_HID::instances[2];
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(receiver);
	usb::publish(receiver);
	uint8_t reply[64];
	std::memset(reply, 0xff, sizeof(reply));
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	for (uint8_t byte : reply) REQUIRE(byte == 0);
	usb::process(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_INPUT, reply, 64) == 1 && reply[0] == 0);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_INPUT, reply, 0) == 0);
	REQUIRE(__wrap_tud_descriptor_bos_cb() == nullptr);
	tusb_control_request_t vendor{0xc0, 1, 0, 2, 255};
	REQUIRE(!__wrap_tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &vendor));

	// Output has no Feature side effects, even if its bytes look like an RPC.
	const uint8_t output[][9] = {
		{0xad, 2, 1, 0}, {0xaf, 4, 'w', 'i', 'p', 'e'},
		{0xf9, 4, 'U', 'F', '2', '!'}, {0x8f, 7, 0, 1, 0, 1, 0, 1, 0}
	};
	for (const auto &body : output) {
		REQUIRE(mockReportData(1, HID_REPORT_TYPE_OUTPUT, body, sizeof(body), sizeof(body)));
		REQUIRE(mockReportAck());
		usb::process(receiver);
		usb::publish(receiver);
		REQUIRE(!receiver.pairing() && !receiver.takeIdentityChange() && !receiver.takeBootloaderRequest());
		REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
		for (uint8_t byte : reply) REQUIRE(byte == 0);
		usb::process(receiver);
	}
	const uint8_t legacy_boot[] = {0x90, 4, 'z', 'o', 't', '!'};
	REQUIRE(!mockFeatureSet(1, legacy_boot, sizeof(legacy_boot), sizeof(legacy_boot)));
	auto rejected = [&](unsigned target, uint8_t command, uint8_t length, uint8_t reason) {
		const uint8_t diagnostics[] = {0xfa, 0};
		REQUIRE(mockFeatureSet(4, diagnostics, 2, 2));
		usb::process(receiver);
		usb::publish(receiver);
		REQUIRE(Adafruit_USBD_HID::instances[4]->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
		REQUIRE(reply[0] == 0xfa && reply[2] == 1 && reply[3] == radio::FIRMWARE_REVISION);
		REQUIRE(reply[10] == command && reply[11] == length && reply[62] == ((target << 6) | reason));
		usb::process(receiver);
	};
	rejected(0, 0x90, 4, 2);
	// D:18e1 -> 4913 seeds the original AES PRNG, not its bootloader.
	// Steam initializes every slot, including slots without a radio peer.
	uint8_t entropy[64] = {0x96, 16};
	for (unsigned i = 2; i < 18; ++i) entropy[i] = uint8_t(0xa0 + i);
	for (unsigned interface_number = 1; interface_number <= 4; ++interface_number) {
		REQUIRE(mockFeatureSet(interface_number, entropy, 64, 64));
		usb::process(receiver);
		usb::publish(receiver);
		REQUIRE(Adafruit_USBD_HID::instances[interface_number]->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
		REQUIRE(reply[0] == 0x96 && reply[1] == 0);
		for (unsigned i = 2; i < 64; ++i) REQUIRE(reply[i] == 0);
		usb::process(receiver);
	}
	REQUIRE(!receiver.pairing() && !receiver.takeBootloaderRequest() && !receiver.takeIdentityChange());
	rejected(0, 0x90, 4, 2); // Successful entropy SETs do not erase rejection history.
	REQUIRE(!mockFeatureSet(3, entropy, 64, 17));
	rejected(2, 0x96, 16, 1);
	REQUIRE(!mockFeatureSet(2, entropy, 64, 64, XFER_RESULT_FAILED));
	rejected(1, 0, 0, 4); // Failed DMA data is not copied into diagnostics.
	REQUIRE(!mockFeatureSet(4, entropy, 1, 1));
	rejected(3, 0, 0, 5);
	const uint8_t bad_selector[] = {0xb1, 1, 0};
	REQUIRE(!mockFeatureSet(3, bad_selector, sizeof(bad_selector), sizeof(bad_selector)));
	rejected(2, 0xb1, 1, 3);
	for (uint8_t command : {uint8_t(0x94), uint8_t(0x95)}) {
		uint8_t body[2] = {command, 0};
		REQUIRE(mockFeatureSet(1, body, 2, 2));
		usb::process(receiver);
		usb::publish(receiver);
		REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
		REQUIRE(reply[0] == 0x94 && reply[1] == 2 && reply[2] == (command == 0x94 ? 0 : 2));
		for (unsigned i = 3; i < 64; ++i) REQUIRE(reply[i] == 0);
		usb::process(receiver);
	}

	const uint8_t status[] = {0xb4, 0};
	uint8_t pairing[] = {0xad, 2, 1, 0};
	for (unsigned i = 0; i < 31; ++i) REQUIRE(mockFeatureSet(2, status, 2, 2));
	REQUIRE(!mockFeatureSet(1, pairing, 4, 4)); // DATA must fail before successful status.
	for (unsigned i = 0; i < 8; ++i) usb::process(receiver);
	REQUIRE(!receiver.pairing());
	REQUIRE(mockFeatureSet(1, pairing, 4, 4));
	usb::process(receiver);
	REQUIRE(receiver.pairing());
	// A write owns a record from DATA through ACK. Other callback producers
	// cannot take its last slot. Protocol work still waits for the loop task.
	pairing[2] = 0;
	REQUIRE(mockReportData(1, HID_REPORT_TYPE_FEATURE, pairing, 4, 4));
	for (unsigned i = 0; i < 31; ++i) other->set(0, HID_REPORT_TYPE_FEATURE, status, 2);
	REQUIRE(mockReportAck());
	REQUIRE(receiver.pairing());
	for (unsigned i = 0; i < 8; ++i) usb::process(receiver);
	REQUIRE(!receiver.pairing());
	// New SETUP cancels a DATA-stage reservation and the unacknowledged write.
	pairing[2] = 1;
	REQUIRE(mockReportData(1, HID_REPORT_TYPE_FEATURE, pairing, 4, 4));
	REQUIRE(mockFeatureSet(2, status, 2, 2));
	usb::process(receiver);
	REQUIRE(!receiver.pairing());

	// A full GET also owns its consumption record until status completion.
	const uint8_t boot[] = {0xf9, 4, 'U', 'F', '2', '!'};
	REQUIRE(mockFeatureSet(1, boot, sizeof(boot), sizeof(boot)));
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64, false) == 64);
	for (unsigned i = 0; i < 31; ++i) other->set(0, HID_REPORT_TYPE_FEATURE, status, 2);
	tusb_control_request_t get{0xa1, 1, 0x0300, 1, 64};
	dcd_edpt0_status_complete(0, &get);
	for (unsigned i = 0; i < 8; ++i) usb::process(receiver);
	REQUIRE(receiver.takeBootloaderRequest());
	REQUIRE(!receiver.takeBootloaderRequest());

	// Bus reset/unplug fences old callbacks immediately; cleanup is deferred.
	// A new request must STALL, not succeed and then be erased by that cleanup.
	for (unsigned event : {DCD_EVENT_BUS_RESET, DCD_EVENT_UNPLUGGED}) {
		REQUIRE(mockFeatureSet(1, pairing, 4, 4)); // Will be cancelled, not executed.
		tud_event_hook_cb(0, event, true);
		usb::publish(receiver);
		REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
		REQUIRE(!mockFeatureSet(1, pairing, 4, 4));
		usb::process(receiver);
		REQUIRE(!receiver.pairing() && !receiver.takeBootloaderRequest());
		REQUIRE(mockFeatureSet(1, pairing, 4, 4));
		usb::process(receiver);
		REQUIRE(receiver.pairing());
		receiver.pairing(false);
	}
	// Reset between GET data and completion cannot consume its old F9 reply.
	REQUIRE(mockFeatureSet(1, boot, sizeof(boot), sizeof(boot)));
	usb::process(receiver);
	usb::publish(receiver);
	REQUIRE(slot->get(0, HID_REPORT_TYPE_FEATURE, reply, 64, false) == 64);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	dcd_edpt0_status_complete(0, &get);
	usb::process(receiver);
	REQUIRE(!receiver.takeBootloaderRequest());
	// Reset between DATA and ACK cannot enqueue the old SET after cleanup.
	REQUIRE(mockReportData(1, HID_REPORT_TYPE_FEATURE, pairing, 4, 4));
	tud_event_hook_cb(0, DCD_EVENT_UNPLUGGED, true);
	usb::process(receiver);
	REQUIRE(!mockReportAck());
	REQUIRE(!receiver.pairing());
	usb::publish(receiver);
	std::puts("PASS: USB report dispatch, startup/fallback GET, ring reservations, aborted SET and reset/unplug fences");
}

void testUsb()
{
	sc::Database db{};
	db.address[1] = 0x42;
	sc::Receiver receiver;
	sc::Hooks hooks = {unusedAes, randomBytes, persist, usb::input, usb::desktop};
	REQUIRE(receiver.begin(db, hooks));
	REQUIRE(usb::begin());
	usb::publish(receiver);
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
	REQUIRE(slot->get(0, HID_REPORT_TYPE_INPUT, reply, 64) == 1 && reply[0] == 0);
	for (unsigned i = 0; i < 40; ++i)
		slot->set(0, HID_REPORT_TYPE_FEATURE, command, 64);
	for (unsigned i = 0; i < 8; ++i)
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
