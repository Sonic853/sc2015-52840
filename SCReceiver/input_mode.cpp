#include "input_mode.h"
#include "protocol.h"
#include <string.h>

namespace input_mode {
static uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
static uint32_t get32(const uint8_t *p) { return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16); }
static void put16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16)); }

Config defaults()
{
	Config c{};
	const uint8_t map[BUTTON_COUNT] = {A, B, X, Y, LB, RB, Back, Start, Guide,
		R3, L3, L3, None, R3, LT, RT};
	memcpy(c.bindings, map, sizeof(map));
	c.stick_deadzone = 3000; c.pad_deadzone = 3500; c.dpad_threshold = 8000;
	c.flags = 4;
	return c;
}
bool valid(const Config &c)
{
	for (uint8_t b : c.bindings) if (b >= TargetCount) return false;
	return !(c.flags & ~7u) && c.stick_deadzone <= 16000 && c.pad_deadzone <= 16000 &&
		c.dpad_threshold >= 256 && c.dpad_threshold <= 30000;
}
void encodeConfig(const Config &c, uint8_t out[CONFIG_SIZE])
{
	out[0] = 1; out[1] = c.flags;
	memcpy(out + 2, c.bindings, BUTTON_COUNT);
	put16(out + 18, c.stick_deadzone); put16(out + 20, c.pad_deadzone);
	put16(out + 22, c.dpad_threshold);
}
bool decodeConfig(const uint8_t in[CONFIG_SIZE], Config &c)
{
	Config next{};
	if (in[0] != 1) return false;
	next.flags = in[1]; memcpy(next.bindings, in + 2, BUTTON_COUNT);
	next.stick_deadzone = get16(in + 18); next.pad_deadzone = get16(in + 20);
	next.dpad_threshold = get16(in + 22);
	if (!valid(next)) return false;
	c = next;
	return true;
}
void encodeRecord(const Config &c, uint32_t generation, uint8_t out[RECORD_SIZE])
{
	memset(out, 0, RECORD_SIZE); memcpy(out, "SCXM", 4);
	put32(out + 4, 1); put32(out + 8, generation); encodeConfig(c, out + 12);
	put32(out + 40, sc::crc32(out, 40)); // final committed word remains zero
}
bool decodeRecord(const uint8_t in[RECORD_SIZE], Config &c, uint32_t &generation)
{
	if (memcmp(in, "SCXM", 4) || get32(in + 4) != 1 || get32(in + 44) ||
	    get32(in + 40) != sc::crc32(in, 40) || !decodeConfig(in + 12, c)) return false;
	generation = get32(in + 8);
	return true;
}

Router::Router() { reset(); }
void Router::reset()
{
	memset(slots_, 0, sizeof(slots_)); config_ = defaults();
	mode_ = request_ = Mode::Steam; pending_ = false; owner_ = -1;
}
void Router::configure(const Config &c)
{
	if (!valid(c)) return;
	config_ = c;
	for (auto &s : slots_) { s.seen = false; s.lx = s.ly = s.lpx = s.lpy = 0; }
}
bool Router::requested(Mode &mode)
{
	if (!pending_) return false;
	mode = request_; pending_ = false;
	return true;
}
void Router::changeMode(Mode mode)
{
	mode_ = mode; pending_ = false;
	if (mode == Mode::Steam) owner_ = -1;
	// Mode changes are global; a held startup chord must not override the UI.
	for (auto &s : slots_) s.consumed = true;
}
void Router::unplug()
{
	changeMode(Mode::Steam);
	for (auto &s : slots_) { s.seen = false; s.suppressed = (s.buttons & 0xa000u) != 0; }
}
bool Router::observe(unsigned index, uint32_t generation, bool connected,
		     const uint8_t report[64], uint32_t now, bool allow_chord)
{
	if (index >= 4) return false;
	Slot &s = slots_[index];
	if (!connected || s.generation != generation) {
		s = {}; s.generation = generation; s.joined_at = now;
		if (owner_ == int(index)) owner_ = -1;
	}
	s.connected = connected;
	if (!connected || report[0] != 1 || report[2] != 1 || report[3] != 60) return false;
	uint32_t buttons = uint32_t(report[8]) | (uint32_t(report[9]) << 8) | (uint32_t(report[10]) << 16);
	bool chord = (buttons & 0xa000u) == 0xa000u;
	if (s.seen && uint32_t(now - s.last_at) > 150000) s.holding = false;
	s.last_at = now; s.seen = true; s.buttons = buttons;
	if (!chord) {
		s.holding = false;
		if (!s.consumed || !(buttons & 0xa000u)) s.suppressed = false;
	}
	else if (s.suppressed || (allow_chord && !s.consumed && mode_ == Mode::Steam && uint32_t(now - s.joined_at) <= 3000000)) {
		s.suppressed = true;
		if (!s.holding) { s.held_at = now; s.holding = true; }
		if (!s.consumed && uint32_t(now - s.held_at) >= 300000) {
			s.consumed = true; request_ = Mode::Xbox; pending_ = true; owner_ = int(index);
		}
	}
	// SC2015 multiplexes left pad and physical stick coordinates. Bit23 says
	// the other source remains active; keep its latest fresh sample only then.
	bool pad_packet = (buttons & (1u << 19)) != 0;
	bool both = (buttons & (1u << 23)) != 0;
	// Legacy firmware aliases the stick press onto the left-pad click when
	// no left pad is active. Normalize before applying user button mappings.
	if (!pad_packet && !both && (buttons & (1u << 17))) {
		s.buttons = (buttons & ~(1u << 17)) | (1u << 22);
	}
	int16_t x = int16_t(get16(report + 16)), y = int16_t(get16(report + 18));
	if (pad_packet) {
		s.lpx = x; s.lpy = y; s.pad_at = now;
		if (!both) s.lx = s.ly = 0;
	} else {
		s.lx = x; s.ly = y; s.stick_at = now;
		if (!both) s.lpx = s.lpy = 0;
	}
	s.rx = (buttons & (1u << 20)) ? int16_t(get16(report + 20)) : 0;
	s.ry = (buttons & (1u << 20)) ? int16_t(get16(report + 22)) : 0;
	s.lt = report[11]; s.rt = report[12];
	if (mode_ == Mode::Xbox && owner_ < 0) owner_ = int(index);
	return s.suppressed;
}
bool Router::suppressDesktop() const
{
	if (mode_ != Mode::Steam) return true;
	for (const auto &s : slots_) if (s.suppressed) return true;
	return false;
}
static int16_t axis(int16_t input, uint16_t deadzone, bool invert = false)
{
	int32_t value = input;
	if (value < -32767) value = -32767;
	if (invert) value = -value;
	int32_t absolute = value < 0 ? -value : value;
	if (absolute <= deadzone) return 0;
	int32_t scaled = (absolute - deadzone) * 32767 / (32767 - deadzone);
	return int16_t(value < 0 ? -scaled : scaled);
}
void Router::xboxReport(uint32_t now, uint8_t out[20])
{
	memset(out, 0, 20); out[1] = 20;
	if (owner_ < 0 || !slots_[owner_].connected) return;
	const Slot &s = slots_[owner_];
	if (!s.seen || s.suppressed || uint32_t(now - s.last_at) > 250000) return;
	static const uint8_t bits[BUTTON_COUNT] = {7,5,6,4,3,2,12,14,13,15,16,22,17,18,1,0};
	static const uint16_t masks[TargetCount] = {0,0x1000,0x2000,0x4000,0x8000,0x0100,0x0200,
		0x0020,0x0010,0x0400,0x0040,0x0080,0x0001,0x0002,0x0004,0x0008,0,0};
	uint16_t buttons = 0;
	out[4] = s.lt; out[5] = s.rt;
	for (unsigned i = 0; i < BUTTON_COUNT; ++i) if (s.buttons & (1u << bits[i])) {
		uint8_t target = config_.bindings[i]; buttons |= masks[target];
		if (target == LT) out[4] = 255;
		if (target == RT) out[5] = 255;
	}
	if ((s.buttons & ((1u << 19) | (1u << 23))) && uint32_t(now - s.pad_at) <= 200000 &&
	    (!(config_.flags & 4) || (s.buttons & (1u << 17)))) {
		uint32_t x = uint32_t(s.lpx < 0 ? -int32_t(s.lpx) : s.lpx);
		uint32_t y = uint32_t(s.lpy < 0 ? -int32_t(s.lpy) : s.lpy);
		uint32_t threshold = config_.dpad_threshold;
		// Radial center deadzone and eight ~45-degree sectors. Testing each
		// axis against the deadzone independently gives radius-dependent angles.
		if (x * x + y * y >= threshold * threshold) {
			if (x * 1000 >= y * 414) buttons |= s.lpx < 0 ? 4 : 8;
			if (y * 1000 >= x * 414) buttons |= s.lpy < 0 ? 2 : 1;
		}
	}
	// Opposing remaps have a neutral result, rather than an invalid D-pad.
	if ((buttons & 3) == 3) buttons &= 0xfffc;
	if ((buttons & 12) == 12) buttons &= 0xfff3;
	put16(out + 2, buttons);
	if (uint32_t(now - s.stick_at) <= 200000) {
		put16(out + 6, uint16_t(axis(s.lx, config_.stick_deadzone)));
		put16(out + 8, uint16_t(axis(s.ly, config_.stick_deadzone)));
	}
	put16(out + 10, uint16_t(axis(s.rx, config_.pad_deadzone, config_.flags & 1)));
	put16(out + 12, uint16_t(axis(s.ry, config_.pad_deadzone, config_.flags & 2)));
}
}
