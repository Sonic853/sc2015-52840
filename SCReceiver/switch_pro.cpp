#include "switch_pro.h"
#include "radio.h"
#include <Arduino.h>
#include <string.h>

namespace switch_pro {
// Switch Pro report schema, cross-checked with OpenPuck's USB descriptor.
// Native drivers interpret the extended packet layout; all replies stay 63 bytes.
static const uint8_t report_descriptor[] = {
	0x05,1,0x09,4,0xa1,1,0x85,0x30,
	0x05,9,0x19,1,0x29,10,0x15,0,0x25,1,0x75,1,0x95,10,0x55,0,0x65,0,0x81,2,
	0x05,9,0x19,11,0x29,14,0x15,0,0x25,1,0x75,1,0x95,4,0x81,2,
	0x75,1,0x95,2,0x81,3,0x0b,1,0,1,0,0xa1,0,
	0x0b,0x30,0,1,0,0x0b,0x31,0,1,0,0x0b,0x32,0,1,0,0x0b,0x35,0,1,0,
	0x15,0,0x27,0xff,0xff,0,0,0x75,16,0x95,4,0x81,2,0xc0,
	0x0b,0x39,0,1,0,0x15,0,0x25,7,0x35,0,0x46,0x3b,1,0x65,0x14,0x75,4,0x95,1,0x81,0x42,
	0x05,9,0x19,15,0x29,18,0x15,0,0x25,1,0x75,1,0x95,4,0x81,2,
	0x75,8,0x95,52,0x81,3,0x06,0,0xff,
	0x85,0x21,0x09,1,0x75,8,0x95,63,0x81,3,0x85,0x81,0x09,2,0x75,8,0x95,63,0x81,3,
	0x85,1,0x09,3,0x75,8,0x95,63,0x91,0x83,0x85,0x10,0x09,4,0x75,8,0x95,63,0x91,0x83,
	0x85,0x80,0x09,5,0x75,8,0x95,63,0x91,0x83,0x85,0x82,0x09,6,0x75,8,0x95,63,0x91,0x83,0xc0,
};
static volatile bool enabled;
static bool streaming, imu_enabled, vibration_enabled;
static uint8_t mac[6], timer, player_lights, prefix[12], latest[63];
static uint8_t user_cal[256], stick_cal[18];
static uint32_t epoch = 1, next_serial, last_stream;
static Report replies[8];
static uint8_t head, count;
static rumble::Mailbox rumble_mailbox;
static int rumble_slot = -1;
static uint32_t rumble_session;
struct Bands { int16_t low, high; };
static Bands bands[2] = {{-256,-256},{-256,-256}};
class Critical {
	uint32_t previous;
public:
	Critical() : previous(__get_PRIMASK()) { __disable_irq(); }
	~Critical() { if (!previous) __enable_irq(); }
};
static uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static uint32_t get32(const uint8_t *p) { return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16); }
static void put32(uint8_t *p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i)); }
static void put16(uint8_t *p, int16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(uint16_t(value) >> 8); }
static void pack12(uint8_t *p, uint16_t x, uint16_t y)
{
	p[0] = uint8_t(x); p[1] = uint8_t((x >> 8) | ((y & 15) << 4)); p[2] = uint8_t(y >> 4);
}
static uint16_t axis(const uint8_t *p)
{
	int32_t value = int16_t(get16(p)); if (value < -32767) value = -32767;
	return uint16_t(2048 + value * 1800 / 32767); // match the factory calibration span
}
static void mapPrefix(const uint8_t mapped[20])
{
	uint16_t b = get16(mapped + 2);
	prefix[1] = 0x80; // full, not charging: virtual status, not handset telemetry
	// Nintendo uses the same physical positions: Xbox A/B/X/Y -> B/A/Y/X.
	prefix[2] = uint8_t(((b & 0x4000) ? 1 : 0) | ((b & 0x8000) ? 2 : 0) |
		((b & 0x1000) ? 4 : 0) | ((b & 0x2000) ? 8 : 0) | ((b & 0x0200) ? 64 : 0) | (mapped[5] >= 40 ? 128 : 0));
	prefix[3] = uint8_t(((b & 0x20) ? 1 : 0) | ((b & 0x10) ? 2 : 0) |
		((b & 0x80) ? 4 : 0) | ((b & 0x40) ? 8 : 0) | ((b & 0x400) ? 16 : 0));
	prefix[4] = uint8_t(((b & 2) ? 1 : 0) | ((b & 1) ? 2 : 0) | ((b & 8) ? 4 : 0) |
		((b & 4) ? 8 : 0) | ((b & 0x100) ? 64 : 0) | (mapped[4] >= 40 ? 128 : 0));
	pack12(prefix + 5, axis(mapped + 6), axis(mapped + 8));
	pack12(prefix + 8, axis(mapped + 10), axis(mapped + 12));
	prefix[11] = 9;
}
static void inputPrefix(uint8_t *out) { memcpy(out, prefix, 12); out[0] = timer++; }
static void resetBands() { for (auto &b : bands) b.low = b.high = -256; }
void stopRumble()
{
	Critical critical; rumble_mailbox.stop(); rumble_slot = -1; rumble_session = 0; resetBands();
}
void reset()
{
	Critical critical;
	if (!++epoch) ++epoch;
	streaming = imu_enabled = vibration_enabled = false; head = count = timer = player_lights = 0;
	last_stream = 0; uint8_t neutral[20] = {0,20}; mapPrefix(neutral);
	memset(latest, 0, sizeof(latest)); memcpy(latest, prefix, 12); stopRumble();
}
void enable(bool value, uint32_t id0, uint32_t id1)
{
	Critical critical; enabled = value;
	mac[0] = 2; put32(mac + 1, id0); mac[5] = uint8_t(id1 ^ (id1 >> 8) ^ (id1 >> 16) ^ (id1 >> 24) ^ 0x53);
	memset(user_cal, 255, sizeof(user_cal));
	pack12(stick_cal, 1800,1800); pack12(stick_cal + 3,2048,2048); pack12(stick_cal + 6,1800,1800);
	pack12(stick_cal + 9,2048,2048); pack12(stick_cal + 12,1800,1800); pack12(stick_cal + 15,1800,1800);
	reset();
}
const uint8_t *reportDescriptor() { return report_descriptor; }
uint16_t reportDescriptorSize() { return sizeof(report_descriptor); }

// Protocol log2 amplitudes are 1/32 units. Integer lookup avoids floating point
// in the USB task. Frequency bands are reduced to an 8-bit peak for SC2015 pulses.
static uint8_t level(int units)
{
	static const uint16_t fraction[32] = {32768,32066,31379,30706,30048,29405,28774,28158,
		27554,26964,26386,25821,25268,24726,24196,23678,23170,22674,22188,21713,21247,
		20792,20347,19911,19484,19066,18658,18258,17867,17484,17109,16743};
	if (units <= -255) return 0;
	unsigned magnitude = unsigned(-units), shift = 15 + (magnitude >> 5);
	return uint8_t((255u * fraction[magnitude & 31] + (1u << (shift - 1))) >> shift);
}
static int16_t absoluteAmplitude(unsigned code)
{
	if (!code) return -256;
	return int16_t(code < 16 ? int(code) * 8 - 248 : code < 32 ? int(code) * 2 - 158 : int(code) - 127);
}
static int16_t relativeAmplitude(unsigned code, int current)
{
	if (!code) return -256;
	if (code <= 11) return int16_t(-16 * int(code - 1));
	int delta = code >= 17 && code <= 19 ? 4 : code >= 20 && code <= 22 ? 1 :
		code >= 26 && code <= 28 ? -1 : code >= 29 ? -4 : 0;
	int value = current + delta;
	return int16_t(value < -256 ? -256 : value > 0 ? 0 : value);
}
static uint8_t decodeMotor(unsigned motor, const uint8_t *data)
{
	uint32_t word = get32(data); Bands &b = bands[motor]; uint8_t peak = 0;
	auto field = [&](unsigned shift, unsigned bits) { return unsigned((word >> shift) & ((1u << bits) - 1)); };
	auto sample = [&]() { uint8_t lo = level(b.low), hi = level(b.high); if (lo > peak) peak = lo; if (hi > peak) peak = hi; };
	auto relative = [&](unsigned lo, unsigned hi) { b.low = relativeAmplitude(field(lo,5), b.low); b.high = relativeAmplitude(field(hi,5), b.high); sample(); };
	switch (word >> 30) {
	case 0: sample(); break;
	case 1:
		if (!(word & 0xfffff)) relative(25,20);
		else if (!(word & 3)) { b.low = absoluteAmplitude(field(23,7)); b.high = absoluteAmplitude(field(9,7)); sample(); }
		else {
			if (!(word & 4)) { if (word & 1) b.high = absoluteAmplitude(field(23,7)); else b.low = absoluteAmplitude(field(23,7)); }
			sample(); relative(18,13); relative(8,3);
		}
		break;
	case 2:
		if (!(word & 0x3ff)) { relative(25,20); relative(15,10); }
		else {
			if (word & 1) { b.high = absoluteAmplitude(field(23,7)); b.low = relativeAmplitude(field(18,5), b.low); }
			else { b.low = absoluteAmplitude(field(23,7)); b.high = relativeAmplitude(field(18,5), b.high); }
			sample(); relative(13,8);
		}
		break;
	case 3: relative(25,20); relative(15,10); relative(5,0); break;
	}
	return peak;
}
static void receiveRumble(const uint8_t *p)
{
	if (!vibration_enabled || !streaming || rumble_slot < 0) { resetBands(); return; }
	uint8_t low = decodeMotor(0, p + 1), high = decodeMotor(1, p + 5);
	// Refresh the mailbox even on hold frames; the bridge, not this decoder,
	// limits RF refresh rate and retains the host-silence stop deadline.
	rumble_mailbox.submit(low, high, radio::queueMicros());
}
static uint8_t spiByte(uint32_t address)
{
	static const uint8_t imu[24] = {0,0,0,0,0,0,0,0x40,0,0x40,0,0x40,0,0,0,0,0,0,0x3b,0x34,0x3b,0x34,0x3b,0x34};
	static const uint8_t parameters[24] = {0x50,0xfd,0,0,0xc6,0x0f,0x0f,0x30,0x61,0x96,0x30,0xf3,
		0xd4,0x14,0x54,0x41,0x15,0x54,0xc7,0x79,0x9c,0x33,0x36,0x63};
	static const uint8_t colors[13] = {0x32,0x32,0x32,0xe6,0xe6,0xe6,0x32,0x32,0x32,0x32,0x32,0x32,0xff};
	if (address >= 0x6020 && address < 0x6038) return imu[address - 0x6020];
	if (address >= 0x603d && address < 0x604f) return stick_cal[address - 0x603d];
	if (address >= 0x6050 && address < 0x605d) return colors[address - 0x6050];
	if (address >= 0x6080 && address < 0x6098) return parameters[address - 0x6080];
	if (address >= 0x6098 && address < 0x60aa) return parameters[address - 0x6098 + 6];
	if (address >= 0x8000 && address < 0x8100) return user_cal[address - 0x8000];
	return 255;
}
static void enqueue(uint8_t id, const uint8_t *body)
{
	Report &r = replies[(head + count) % 8]; r.id = id; memcpy(r.data, body, 63); r.epoch = epoch;
	if (!++next_serial) ++next_serial;
	r.serial = next_serial; ++count;
}
uint8_t minimumOutputSize(uint8_t id)
{
	return id == 0x80 || id == 0x82 ? 1 : id == 0x10 ? 9 : id == 1 ? 10 : 0;
}
bool validOutput(uint8_t id, const uint8_t *p, uint16_t size)
{
	uint8_t minimum = minimumOutputSize(id);
	if (!p || !minimum || size < minimum || size > 63) return false;
	if (id == 0x80) return p[0] >= 1 && p[0] <= 6;
	if (id != 1) return true;
	unsigned args = size - 10;
	switch (p[9]) {
	case 1: case 3: case 6: case 8: case 0x30: case 0x38: case 0x40: case 0x48: return args >= 1;
	case 0x10: return args >= 5;
	case 0x11: return args >= 5 && unsigned(p[14]) <= args - 5;
	case 0x41: return args >= 4;
	default: return true;
	}
}
static void subcommand(const uint8_t *p)
{
	uint8_t out[63] = {}; inputPrefix(out); uint8_t sub = p[9]; const uint8_t *args = p + 10;
	out[12] = 0x80; out[13] = sub;
	switch (sub) {
	case 1: // Wired registration replies; this does not pair a Bluetooth radio.
		out[12] = 0x81; out[14] = args[0];
		if (args[0] == 1) {
			memcpy(out + 15, mac, 6); out[22] = 0x25; out[23] = 8;
			memcpy(out + 24, "Pro Controller", 14); out[43] = 0x68;
		} else if (args[0] == 2) {
			// Synthetic USB-registration bytes, unrelated to any SC2015 key.
			for (unsigned i = 0; i < 16; ++i) out[15 + i] = uint8_t(mac[i % 6] ^ (0x5d + i * 17));
		} else if (args[0] != 3) out[12] = 0;
		break;
	case 2:
		out[12] = 0x82; out[14] = 3; out[15] = 0x48; out[16] = 3; out[17] = 2;
		memcpy(out + 18, mac, 6); out[24] = out[25] = 1; break;
	case 3:
		if (args[0] == 0x30) streaming = true;
		else if (args[0] == 0) { streaming = false; stopRumble(); }
		else out[12] = 0;
		break;
	case 4: out[12] = 0x83; break; // no measured trigger elapsed-time telemetry
	case 6: case 8: case 0x38: break; // power/LED configuration accepted as no-op
	case 0x10: {
		uint32_t address = get32(args); unsigned n = args[4] > 29 ? 29 : args[4];
		out[12] = 0x90; memcpy(out + 14, args, 4); out[18] = uint8_t(n);
		for (unsigned i = 0; i < n; ++i) out[19 + i] = spiByte(address + i);
		break;
	}
	case 0x11: {
		uint32_t address = get32(args); unsigned n = args[4];
		if (n > 29 || address < 0x8000 || address >= 0x8100 || n > 0x8100 - address) { out[12] = 0; out[14] = 1; }
		else memcpy(user_cal + address - 0x8000, args + 5, n); // RAM only, never receiver Flash
		break;
	}
	case 0x30: player_lights = args[0]; break;
	case 0x21: out[12] = 0xa0; break; // NFC configuration compatibility ACK; no NFC data channel
	case 0x31: out[12] = 0xb0; out[14] = player_lights; break;
	case 0x40: if (args[0] <= 1) imu_enabled = args[0] != 0; else out[12] = 0; break;
	case 0x41: break; // fixed calibrated output scale; source sensor configuration stays owned by the receiver
	case 0x48:
		if (args[0] > 1) out[12] = 0;
		else { vibration_enabled = args[0] != 0; if (!vibration_enabled) stopRumble(); }
		break;
	default: out[12] = 0; break;
	}
	enqueue(0x21, out);
}
void setReport(uint8_t id, hid_report_type_t type, const uint8_t *p, uint16_t size)
{
	if (!enabled || type != HID_REPORT_TYPE_OUTPUT || !p) return;
	if (!id) { if (!size) return; id = *p++; --size; }
	if (!validOutput(id, p, size)) return;
	Critical critical;
	if (!enabled || !TinyUSBDevice.mounted() || TinyUSBDevice.suspended()) return;
	bool reply = id == 1 || (id == 0x80 && p[0] <= 3);
	if (reply && count == 8) return; // drop the entire request; host can retry on response timeout
	if (id == 0x80) {
		if (p[0] <= 3) {
			uint8_t out[63] = {}; out[0] = p[0];
			if (p[0] == 1) { out[2] = 3; memcpy(out + 3, mac, 6); }
			enqueue(0x81, out);
		} else if (p[0] == 6) reset();
	} else if (id == 1) { subcommand(p); receiveRumble(p); }
	else if (id == 0x10) receiveRumble(p);
}
bool nextReport(const uint8_t mapped[20], uint32_t now, Report &r, const motion::Sample *sample)
{
	Critical critical;
	if (!enabled) return false;
	mapPrefix(mapped);
	if (count) { r = replies[head]; return true; }
	if (!streaming || uint32_t(now - last_stream) < (imu_enabled ? 15000u : 4000u)) return false;
	r.id = 0x30; r.epoch = epoch; r.serial = 0;
	memset(r.data, 0, 63); inputPrefix(r.data);
	if (imu_enabled && sample) {
		motion::Axes axes = motion::switchPro(*sample);
		// Zero-order hold across three 5ms slots. These are not three new
		// handset samples; ordering is identical for SDL and Linux readers.
		for (unsigned i = 0; i < 3; ++i) for (unsigned axis = 0; axis < 3; ++axis) {
			put16(r.data + 12 + 12*i + 2*axis, axes.accel[axis]);
			put16(r.data + 18 + 12*i + 2*axis, axes.gyro[axis]);
		}
	}
	memcpy(latest, r.data, 63); return true;
}
bool current(const Report &r)
{
	Critical critical;
	return enabled && r.epoch == epoch && (!r.serial || (count && replies[head].serial == r.serial));
}
void sent(const Report &r, uint32_t now)
{
	Critical critical;
	if (!current(r)) return;
	if (r.serial) { head = (head + 1) % 8; --count; }
	else last_stream = now;
}
uint16_t getReport(uint8_t id, hid_report_type_t type, uint8_t *out, uint16_t capacity)
{
	Critical critical;
	if (!enabled || id != 0x30 || type != HID_REPORT_TYPE_INPUT || !out) return 0;
	uint16_t n = capacity < 63 ? capacity : 63; memcpy(out, latest, n); return n;
}
uint32_t setRumbleTarget(int slot, uint32_t session)
{
	Critical critical;
	bool active = enabled && streaming && vibration_enabled && TinyUSBDevice.mounted() && !TinyUSBDevice.suspended();
	uint32_t token = rumble_mailbox.target(slot, session, active);
	int selected = token ? slot : -1;
	if (selected != rumble_slot || session != rumble_session) resetBands();
	rumble_slot = selected; rumble_session = session; return token;
}
bool takeRumble(rumble::Command &command) { Critical critical; return rumble_mailbox.take(command); }
}
