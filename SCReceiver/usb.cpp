#include "usb.h"
#include "radio.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <string.h>

#if CFG_TUD_HID < 5
#error "Five HID instances are required for the original receiver layout"
#endif

namespace usb {

// Recovered descriptors at D:0a2a/0aa3. No report ID belongs on a slot's
// 64-byte vendor reports; keyboard/mouse on interface zero use IDs 1/2.
static const uint8_t desktop_descriptor[] = {
	0x05,0x01,0x09,0x06,0x95,0x01,0xa1,0x01,0x85,0x01,0x05,0x07,
	0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,
	0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,
	0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,
	0x91,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,
	0x19,0x00,0x29,0x65,0x81,0x00,0xc0,0x05,0x01,0x09,0x02,0xa1,
	0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,
	0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,
	0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,
	0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0xc0,0xc0,
};
static const uint8_t slot_descriptor[] = {
	0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x15,0x00,0x26,0xff,
	0x00,0x75,0x08,0x95,0x40,0x09,0x01,0x81,0x02,0x95,0x40,
	0x09,0x01,0x91,0x02,0x95,0x40,0x09,0x01,0xb1,0x02,0xc0,
};

class Hid : public Adafruit_USBD_HID {
public:
	bool is_desktop = false;
	uint16_t getInterfaceDescriptor(uint8_t number, uint8_t *buffer,
				       uint16_t capacity) override
	{
		uint16_t count = Adafruit_USBD_HID::getInterfaceDescriptor(
			number, buffer, capacity);
		if (buffer && count >= 25) {
			buffer[11] = 0x11;
			buffer[12] = 0x01;
			if (is_desktop) {
				buffer[22] = 9;
				buffer[23] = 0;
			}
		}
		return count;
	}
};

static Hid hid[5];
static uint8_t configuration[256];

struct Command {
	bool get;
	uint8_t slot, size, data[64];
	uint32_t version;
};
static Command commands[32];
static volatile uint8_t command_head, command_tail;
static volatile uint8_t pending_sets[4];
static uint8_t replies[4][64];
static uint32_t reply_versions[4];
static volatile uint32_t command_drops;
// EP0 has one control transfer at a time. Capture the exact published version
// at SETUP, but consume it only after TinyUSB completes the status stage.
static bool feature_read_pending;
static uint8_t feature_read_slot;
static uint32_t feature_read_version;

struct Output {
	uint8_t data[8][64];
	uint8_t head, count;
};
static Output outputs[4];
static uint8_t keyboard_state[8];
static bool keyboard_dirty;
static uint8_t mouse_buttons;
static bool mouse_dirty;
static int mouse_motion[3];

class Critical {
	uint32_t previous;
public:
	Critical() : previous(__get_PRIMASK()) { __disable_irq(); }
	~Critical() { if (!previous) __enable_irq(); }
};

template<unsigned Slot>
static uint16_t getReport(uint8_t id, hid_report_type_t type,
			  uint8_t *buffer, uint16_t requested)
{
	if (id || type != HID_REPORT_TYPE_FEATURE || !requested)
		return 0;
	Critical critical;
	feature_read_pending = false;
	unsigned count = requested < 64 ? requested : 64;
	memcpy(buffer, replies[Slot], count);
	// Descriptor/header probes must not consume a reply or arm a reset.
	if (count < 64)
		return uint16_t(count);
	feature_read_slot = Slot;
	feature_read_version = reply_versions[Slot];
	feature_read_pending = true;
	return uint16_t(count);
}

static void controlComplete(const tusb_control_request_t &request)
{
	Critical critical;
	bool consume = feature_read_pending && request.bmRequestType == 0xa1 &&
		request.bRequest == 1 && request.wValue == 0x0300 &&
		request.wLength >= 64 && request.wIndex == feature_read_slot + 1;
	feature_read_pending = false;
	if (!consume)
		return;
	uint8_t next = (command_head + 1) % 32;
	if (next == command_tail) {
		++command_drops;
		return;
	}
	Command &command = commands[command_head];
	command.get = true;
	command.slot = feature_read_slot;
	command.version = feature_read_version;
	__DMB();
	command_head = next;
}

template<unsigned Slot>
static void setReport(uint8_t id, hid_report_type_t type,
		      const uint8_t *buffer, uint16_t count)
{
	if (id || (type != HID_REPORT_TYPE_FEATURE && type != HID_REPORT_TYPE_OUTPUT) ||
	    count < 2 || count > 64 || buffer[1] > 62 || buffer[1] + 2 > count)
		return;
	Critical critical;
	uint8_t next = (command_head + 1) % 32;
	if (next == command_tail) {
		++command_drops;
		return;
	}
	Command &command = commands[command_head];
	command.get = false;
	command.slot = Slot;
	command.size = uint8_t(count);
	memcpy(command.data, buffer, count);
	++pending_sets[Slot];
	memset(replies[Slot], 0, 64);
	replies[Slot][0] = buffer[0];
	reply_versions[Slot] = 0;
	__DMB();
	command_head = next;
}

bool begin()
{
	TinyUSBDevice.detach();
	delay(30);
	TinyUSBDevice.clearConfiguration();
	TinyUSBDevice.setConfigurationBuffer(configuration, sizeof(configuration));
	TinyUSBDevice.setID(0x28de, 0x1142);
	TinyUSBDevice.setVersion(0x0200);
	TinyUSBDevice.setDeviceVersion(0x0001);
	TinyUSBDevice.setManufacturerDescriptor("Valve Software");
	TinyUSBDevice.setProductDescriptor("Steam Controller");
	hid[0].is_desktop = true;
	hid[0].setReportDescriptor(desktop_descriptor, sizeof(desktop_descriptor));
	hid[0].setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
	hid[0].setPollInterval(2);
	if (!hid[0].begin())
		return false;
	hid[1].setReportCallback(getReport<0>, setReport<0>);
	hid[2].setReportCallback(getReport<1>, setReport<1>);
	hid[3].setReportCallback(getReport<2>, setReport<2>);
	hid[4].setReportCallback(getReport<3>, setReport<3>);
	for (unsigned i = 1; i < 5; ++i) {
		hid[i].setReportDescriptor(slot_descriptor, sizeof(slot_descriptor));
		hid[i].setPollInterval(6);
		if (!hid[i].begin())
			return false;
	}
	TinyUSBDevice.setConfigurationAttribute(0xa0);
	TinyUSBDevice.setConfigurationMaxPower(100);
	return TinyUSBDevice.attach();
}

void process(sc::Receiver &receiver)
{
	// USB callbacks only copy bounded records. Flash writes, crypto, and
	// protocol state transitions belong to the loop task, never the USB task.
	for (unsigned i = 0; i < 16; ++i) {
		Command command;
		{
			Critical critical;
			if (command_tail == command_head)
				break;
			command = commands[command_tail];
			command_tail = (command_tail + 1) % 32;
		}
		if (command.get)
			receiver.consumeFeature(command.slot, command.version);
		else {
			receiver.feature(command.slot, command.data, command.size);
			Critical critical;
			--pending_sets[command.slot];
		}
	}
}

static void put32(uint8_t *out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out[i] = uint8_t(value >> (8 * i));
}

void publish(const sc::Receiver &receiver)
{
	for (unsigned slot = 0; slot < 4; ++slot) {
		uint8_t snapshot[64];
		uint32_t version;
		receiver.peekFeature(slot, snapshot, version);
		if (snapshot[0] == 0xf0) {
			const radio::Statistics &s = radio::statistics();
			snapshot[1] = 62;
			put32(snapshot + 35, s.crc_ok);
			put32(snapshot + 39, s.crc_bad);
			put32(snapshot + 43, s.queue_full);
			put32(snapshot + 47, s.late_ack);
			put32(snapshot + 51, s.faults);
			put32(snapshot + 55, command_drops);
		}
		if (snapshot[0] == 0xf7)
			radio::pairingDiagnostics(snapshot);
		Critical critical;
		if (!pending_sets[slot]) {
			memcpy(replies[slot], snapshot, 64);
			reply_versions[slot] = version;
		}
	}
}

void input(unsigned slot, const uint8_t report[64])
{
	if (slot >= 4)
		return;
	Output &output = outputs[slot];
	if (output.count == 8) {
		// Coalesce superseded input, but retain connection notifications when
		// possible. Transport acknowledgement does not wait for USB readiness.
		unsigned last = (output.head + output.count - 1) % 8;
		if (report[2] == 1 && output.data[last][2] == 1) {
			memcpy(output.data[last], report, 64);
			return;
		}
		output.head = (output.head + 1) % 8;
		--output.count;
	}
	memcpy(output.data[(output.head + output.count) % 8], report, 64);
	++output.count;
}

void desktop(const uint8_t keyboard[8], const uint8_t mouse[4])
{
	if (memcmp(keyboard, keyboard_state, 8)) {
		memcpy(keyboard_state, keyboard, 8);
		keyboard_dirty = true;
	}
	if (mouse_buttons != mouse[0])
		mouse_dirty = true;
	mouse_buttons = mouse[0];
	for (unsigned i = 0; i < 3; ++i) {
		mouse_motion[i] += int8_t(mouse[i + 1]);
		if (mouse_motion[i] > 32767)
			mouse_motion[i] = 32767;
		if (mouse_motion[i] < -32767)
			mouse_motion[i] = -32767;
		mouse_dirty |= mouse_motion[i] != 0;
	}
}

void pump()
{
	if (!TinyUSBDevice.mounted()) {
		for (auto &output : outputs)
			output.head = output.count = 0;
		return;
	}
	if (TinyUSBDevice.suspended())
		return;
	for (unsigned i = 0; i < 4; ++i) {
		Output &output = outputs[i];
		if (output.count && hid[i + 1].ready() &&
		    hid[i + 1].sendReport(0, output.data[output.head], 64)) {
			output.head = (output.head + 1) % 8;
			--output.count;
		}
	}
	if (keyboard_dirty && hid[0].ready() &&
	    hid[0].sendReport(1, keyboard_state, 8))
		keyboard_dirty = false;
	if (mouse_dirty && hid[0].ready()) {
		uint8_t report[4] = {mouse_buttons};
		for (unsigned i = 0; i < 3; ++i)
			report[i + 1] = uint8_t(mouse_motion[i] < -127 ? -127 :
					      mouse_motion[i] > 127 ? 127 : mouse_motion[i]);
		if (hid[0].sendReport(2, report, 4)) {
			mouse_dirty = false;
			for (unsigned i = 0; i < 3; ++i) {
				mouse_motion[i] -= int8_t(report[i + 1]);
				mouse_dirty |= mouse_motion[i] != 0;
			}
		}
	}
}

} // namespace usb

// TinyUSB's nRF5 DCD has no platform implementation of this weak hook.
// It runs after EP0 status completion, unlike tud_hid_get_report_cb (SETUP).
extern "C" void dcd_edpt0_status_complete(uint8_t, const tusb_control_request_t *request)
{
	usb::controlComplete(*request);
}

extern "C" const uint8_t *__real_tud_descriptor_device_cb(void);
extern "C" const uint8_t *__wrap_tud_descriptor_device_cb(void)
{
	static tusb_desc_device_t descriptor;
	memcpy(&descriptor, __real_tud_descriptor_device_cb(), sizeof(descriptor));
	descriptor.bDeviceClass = descriptor.bDeviceSubClass = descriptor.bDeviceProtocol = 0;
	descriptor.iSerialNumber = 0;
	return reinterpret_cast<const uint8_t *>(&descriptor);
}
