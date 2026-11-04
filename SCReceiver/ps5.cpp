#include "ps5.h"
#include "radio.h"
#include <Arduino.h>
#include <string.h>

namespace ps5 {
// DualSense USB field layout, cross-checked against OpenPuck mode_ps5.cpp and
// SDL_hidapi_ps5.c. The descriptor declares only the reports implemented here.
static const uint8_t report_descriptor[] = {
	0x05,1, 0x09,5, 0xa1,1, 0x85,1,
	0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35, 0x09,0x33, 0x09,0x34,
	0x15,0, 0x26,0xff,0, 0x75,8, 0x95,6, 0x81,2,
	0x06,0,0xff, 0x09,0x20, 0x95,1, 0x81,2,
	0x05,1, 0x09,0x39, 0x15,0, 0x25,7, 0x35,0, 0x46,0x3b,1,
	0x65,0x14, 0x75,4, 0x95,1, 0x81,0x42, 0x65,0,
	0x05,9, 0x19,1, 0x29,15, 0x15,0, 0x25,1, 0x75,1, 0x95,15, 0x81,2,
	0x06,0,0xff, 0x09,0x21, 0x95,13, 0x81,2,
	0x09,0x22, 0x15,0, 0x26,0xff,0, 0x75,8, 0x95,52, 0x81,2,
	0x85,2, 0x09,0x23, 0x95,47, 0x91,2,
	0x85,3, 0x09,0x24, 0x95,47, 0xb1,2,
	0x85,5, 0x09,0x33, 0x95,40, 0xb1,2,
	0x85,9, 0x09,0x24, 0x95,19, 0xb1,2,
	0x85,0x20, 0x09,0x26, 0x95,63, 0xb1,2, 0xc0,
};
static volatile bool enabled;
static uint8_t mac[6], latest[63];
static uint32_t sequence;
static rumble::Mailbox rumble_mailbox;
class Critical {
	uint32_t previous;
public:
	Critical() : previous(__get_PRIMASK()) { __disable_irq(); }
	~Critical() { if (!previous) __enable_irq(); }
};
static uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static void put16(uint8_t *p, int16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(uint16_t(v) >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
static uint8_t axis(const uint8_t *p, bool invert)
{
	int32_t v = int16_t(get16(p));
	if (invert) v = -v;
	if (v > 32767) v = 32767;
	return uint8_t((v + 32768) / 256);
}
static void neutral(uint8_t out[63])
{
	memset(out, 0, 63); memset(out, 128, 4); out[7] = 8;
	out[32] = out[36] = 0x80; // no touch contacts: pads are D-pad/right stick
	out[52] = 0x1a; out[53] = 0x08; // USB power/status placeholder, not RF battery telemetry
}
const uint8_t *reportDescriptor() { return report_descriptor; }
uint16_t reportDescriptorSize() { return sizeof(report_descriptor); }
void reset()
{
	Critical critical; sequence = 0; neutral(latest); rumble_mailbox.stop();
}
void enable(bool value, uint32_t id0, uint32_t id1)
{
	Critical critical;
	enabled = value;
	// Stable locally-administered unicast identity, distinct from RF bonds.
	mac[0] = 2; put32(mac + 1, id0); mac[5] = uint8_t(id1 ^ (id1 >> 8) ^ (id1 >> 16) ^ (id1 >> 24));
	reset();
}
void buildReport(const uint8_t mapped[20], uint32_t now, uint8_t out[63], const motion::Sample *sample)
{
	neutral(out);
	uint16_t buttons = get16(mapped + 2);
	out[0] = axis(mapped + 6, false); out[1] = axis(mapped + 8, true);
	out[2] = axis(mapped + 10, false); out[3] = axis(mapped + 12, true);
	out[4] = mapped[4]; out[5] = mapped[5];
	static const uint8_t hats[16] = {8,0,4,8,6,7,5,8,2,1,3,8,8,8,8,8};
	out[7] = uint8_t(hats[buttons & 15] | ((buttons & 0x4000) ? 0x10 : 0) |
		((buttons & 0x1000) ? 0x20 : 0) | ((buttons & 0x2000) ? 0x40 : 0) | ((buttons & 0x8000) ? 0x80 : 0));
	out[8] = uint8_t(((buttons & 0x0100) ? 1 : 0) | ((buttons & 0x0200) ? 2 : 0) |
		(out[4] ? 4 : 0) | (out[5] ? 8 : 0) | ((buttons & 0x0020) ? 16 : 0) |
		((buttons & 0x0010) ? 32 : 0) | ((buttons & 0x0040) ? 64 : 0) | ((buttons & 0x0080) ? 128 : 0));
	out[9] = (buttons & 0x0400) ? 1 : 0;
	if (sample) {
		motion::Axes axes = motion::dualsense(*sample);
		for (unsigned i = 0; i < 3; ++i) { put16(out + 15 + 2*i, axes.gyro[i]); put16(out + 21 + 2*i, axes.accel[i]); }
	}
	put32(out + 27, now * 3u);
	Critical critical;
	out[6] = uint8_t(sequence); put32(out + 11, sequence++);
	memcpy(latest, out, 63);
}
uint16_t getReport(uint8_t id, hid_report_type_t type, uint8_t *out, uint16_t capacity)
{
	if (!enabled || !out || !capacity) return 0;
	if (type == HID_REPORT_TYPE_INPUT && id == 1) {
		uint16_t n = capacity < 63 ? capacity : 63;
		Critical critical; memcpy(out, latest, n); return n;
	}
	if (type != HID_REPORT_TYPE_FEATURE) return 0;
	uint8_t payload[63] = {};
	uint16_t size;
	switch (id) {
	case 3:
		size = 47; payload[1] = 0x28; payload[2] = 1; payload[3] = 10; break;
	case 5:
		size = 40;
		// 1000 / 16384 deg/s per count exactly matches SC2015's 2000/32768.
		for (unsigned offset = 6; offset <= 14; offset += 4) { put16(payload + offset, 8192); put16(payload + offset + 2, -8192); }
		put16(payload + 18, 500); put16(payload + 20, 500);
		for (unsigned offset = 22; offset <= 30; offset += 4) { put16(payload + offset, 8192); put16(payload + offset + 2, -8192); }
		break;
	case 9:
		size = 19; memcpy(payload, mac, 6); break;
	case 0x20:
		size = 63;
		memcpy(payload, "Nov  4 2026" "00:00:00", 19);
		put32(payload + 23, 1); put32(payload + 27, 0x0224);
		put16(payload + 43, 0x0224); // USB compatibility profile, not the receiver revision
		break;
	default: return 0;
	}
	if (size > capacity) size = capacity;
	memcpy(out, payload, size); return size;
}
void setReport(uint8_t id, hid_report_type_t type, const uint8_t *data, uint16_t length)
{
	if (!enabled || type != HID_REPORT_TYPE_OUTPUT || !data) return;
	if (!id) { // Interrupt OUT includes ID; control SET_REPORT already stripped it.
		if (length < 48 || length > 64 || data[0] != 2) return;
		id = *data++; --length;
	}
	if (id != 2 || length < 47 || length > 63) return;
	// SDL sends ID + 47 common bytes; Linux adds fifteen reserved bytes.
	// Admit complete common bodies plus zero padding, never a partial body.
	for (unsigned i = 47; i < length; ++i) if (data[i]) return;
	uint8_t low, high;
	if ((data[0] & 1) || (data[38] & 4)) { low = data[3]; high = data[2]; }
	else if ((data[0] & 2) || (!data[0] && !data[1] && !data[38])) low = high = 0;
	else return; // LED/adaptive-trigger-only writes must not overwrite rumble.
	Critical critical;
	if (enabled && TinyUSBDevice.mounted() && !TinyUSBDevice.suspended())
		rumble_mailbox.submit(low, high, radio::queueMicros());
}
uint32_t setRumbleTarget(int slot, uint32_t session)
{
	Critical critical;
	return rumble_mailbox.target(slot, session, enabled && TinyUSBDevice.mounted() && !TinyUSBDevice.suspended());
}
bool takeRumble(rumble::Command &command) { Critical critical; return rumble_mailbox.take(command); }
void stopRumble() { Critical critical; rumble_mailbox.stop(); }
}
