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
	c.flags = 4 | (6 << 4); // click required, 30-degree diagonal sectors
	return c;
}
uint8_t diagonalDegrees(const Config &config)
{
	uint8_t code = config.flags >> 4;
	// Zero preserves the shipped 45-degree geometry. Code nine is reassigned
	// to zero degrees, so all sixteen 5-degree settings have unique encodings.
	return code == 0 ? 45 : code == 9 ? 0 : uint8_t(code * 5);
}
bool setDiagonalDegrees(Config &config, uint8_t degrees)
{
	if (degrees > 75 || degrees % 5) return false;
	uint8_t code = degrees == 45 ? 0 : degrees == 0 ? 9 : uint8_t(degrees / 5);
	config.flags = uint8_t((config.flags & 15) | (code << 4));
	return true;
}
bool valid(const Config &c)
{
	for (uint8_t b : c.bindings) if (b >= TargetCount) return false;
	return c.stick_deadzone <= 16000 && c.pad_deadzone <= 16000 &&
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
BootState defaultBootState() { return {Mode::Steam, NO_BOOT_MODE}; }
BootState bootRequest(const Config &config, Mode mode)
{
	return {mode, uint8_t(config.flags & PERSIST_MODE ? NO_BOOT_MODE : uint8_t(mode))};
}
Mode consumeBoot(const Config &config, BootState &boot)
{
	Mode mode = validMode(boot.once) ? Mode(boot.once) : (config.flags & PERSIST_MODE ? boot.last : Mode::Steam);
	boot.once = NO_BOOT_MODE;
	return mode;
}
void encodeRecord(const Config &c, uint32_t generation, uint8_t out[RECORD_SIZE], BootState boot)
{
	memset(out, 0, RECORD_SIZE); memcpy(out, "SCXM", 4);
	put32(out + 4, 2); put32(out + 8, generation); encodeConfig(c, out + 12);
	out[36] = uint8_t(boot.last); out[37] = boot.once;
	put32(out + 40, sc::crc32(out, 40)); // final committed word remains zero
}
bool decodeRecord(const uint8_t in[RECORD_SIZE], Config &c, uint32_t &generation, BootState *boot)
{
	uint32_t version = get32(in + 4);
	if (memcmp(in, "SCXM", 4) || (version != 1 && version != 2) || get32(in + 44) ||
	    get32(in + 40) != sc::crc32(in, 40) || !decodeConfig(in + 12, c)) return false;
	BootState state = defaultBootState();
	if (version == 1) {
		if (c.flags & ~7u) return false;
	} else {
		if (!validMode(in[36]) || (!validMode(in[37]) && in[37] != NO_BOOT_MODE) || in[38] || in[39]) return false;
		state = {Mode(in[36]), in[37]};
	}
	if (boot) *boot = state;
	generation = get32(in + 8);
	return true;
}

Router::Router() { reset(); }
void Router::reset()
{
	for (auto &s : slots_) s = {};
	config_ = defaults();
	mode_ = request_ = Mode::Steam; pending_ = false; owner_ = -1;
	lizard_.reset();
}
void Router::configure(const Config &c)
{
	if (!valid(c)) return;
	config_ = c;
	lizard_.reset();
	for (auto &s : slots_) { s.seen = false; s.lx = s.ly = s.lpx = s.lpy = 0; s.motion.reset(); }
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
	clearMotion();
	if (mode == Mode::Steam) owner_ = -1;
	// Mode changes are global; a held startup chord must not override the UI.
	for (auto &s : slots_) s.consumed = true;
}
void Router::clearMotion()
{
	// USB lifecycle fences must discard sensor history without treating a
	// synthetic neutral report as release of a physical startup chord.
	for (auto &s : slots_) s.motion.reset();
	lizard_.reset();
}
void Router::unplug()
{
	changeMode(Mode::Steam);
	for (auto &s : slots_) {
		s.seen = false; s.chord_mask = 0x1a088u; s.suppressed = (s.buttons & s.chord_mask) != 0;
	}
}
bool Router::observe(unsigned index, uint32_t generation, bool connected,
		     const uint8_t report[64], uint32_t now, bool allow_chord, uint8_t motion_updated)
{
	if (index >= 4) return false;
	Slot &s = slots_[index];
	if (!connected || s.generation != generation) {
		s = {}; s.generation = generation; s.joined_at = now;
		if (owner_ == int(index)) { owner_ = -1; lizard_.reset(); }
	}
	s.connected = connected;
	if (!connected || report[0] != 1 || report[2] != 1 || report[3] != 60) return false;
	s.motion.observe(report, motion_updated, now);
	uint32_t buttons = uint32_t(report[8]) | (uint32_t(report[9]) << 8) | (uint32_t(report[10]) << 16);
	// A+Steam always wins. The two rear selectors together explicitly select
	// Lizard; mixing the shoulder selector with a rear selector is ambiguous.
	uint32_t chord = (buttons & 0x2080u) == 0x2080u ? 0x2080u :
		(buttons & 0x1a008u) == 0x1a000u ? 0x1a000u :
		(buttons & 0x1a008u) == 0x2008u ? 0x2008u :
		(buttons & 0x1a008u) == 0x12000u ? 0x12000u :
		(buttons & 0x1a008u) == 0xa000u ? 0xa000u : 0;
	Mode target = chord == 0x2080u ? Mode::Steam : chord == 0x2008u ? Mode::Switch :
		chord == 0x12000u ? Mode::PS5 : chord == 0x1a000u ? Mode::Lizard : Mode::Xbox;
	if (s.seen && uint32_t(now - s.last_at) > 150000) s.holding = false;
	s.last_at = now; s.seen = true; s.buttons = buttons;
	if (s.suppressed && !(buttons & s.chord_mask)) s.suppressed = false;
	if (!chord) {
		s.holding = false;
	}
	else if (allow_chord && !s.consumed && uint32_t(now - s.joined_at) <= 3000000) {
		s.suppressed = true;
		if (!s.holding || s.chord_mask != chord) { s.held_at = now; s.holding = true; }
		s.chord_mask = chord;
		if (!s.consumed && uint32_t(now - s.held_at) >= 300000) {
			s.consumed = true;
			if (target != mode_) { request_ = target; pending_ = true;
				if (target != Mode::Steam) owner_ = int(index);
			}
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
	if (mode_ != Mode::Steam && owner_ < 0) { owner_ = int(index); lizard_.reset(); }
	if (mode_ == Mode::Lizard && owner_ == int(index)) {
		if (s.suppressed || !allow_chord) lizard_.reset();
		else lizard_.observe({s.buttons, s.stick_at, s.lx, s.ly, s.lpx, s.lpy, s.rx, s.ry,
			s.lt, s.rt, pad_packet, pad_packet || both}, now);
	}
	return s.suppressed;
}
bool Router::suppressDesktop() const
{
	if (mode_ != Mode::Steam) return true;
	for (const auto &s : slots_) if (s.suppressed) return true;
	return false;
}
int Router::rumbleOwner(uint32_t now) const
{
	if (mode_ == Mode::Steam || owner_ < 0) return -1;
	const Slot &s = slots_[owner_];
	return s.connected && s.seen && !s.suppressed && uint32_t(now - s.last_at) <= 250000 ? owner_ : -1;
}
motion::Sample Router::motionSample(uint32_t now) const
{
	if (rumbleOwner(now) < 0) return {};
	return slots_[owner_].motion.read(now);
}
lizard::Report Router::lizardReport(uint32_t now)
{
	if (mode_ != Mode::Lizard || rumbleOwner(now) < 0) { lizard_.reset(); return {}; }
	return lizard_.report(now);
}
void Router::lizardMouseSent(int x, int y, int wheel) { lizard_.mouseSent(x, y, wheel); }
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
		// tan((90 - diagonal_degrees) / 2), scaled by 10000. Separate angular
		// boundaries from the radial deadzone so sectors keep their width.
		static const uint16_t ratios[16] = {4142,9163,8391,7673,7002,6371,5774,5206,
			4663,10000,3640,3153,2679,2217,1763,1317};
		uint8_t code = config_.flags >> 4;
		if (x * x + y * y >= threshold * threshold) {
			if (code == 9) { // no diagonal sector; exact ties choose horizontal
				buttons |= x >= y ? (s.lpx < 0 ? 4 : 8) : (s.lpy < 0 ? 2 : 1);
			} else {
				if (x * 10000 >= y * ratios[code]) buttons |= s.lpx < 0 ? 4 : 8;
				if (y * 10000 >= x * ratios[code]) buttons |= s.lpy < 0 ? 2 : 1;
			}
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
