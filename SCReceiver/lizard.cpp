#include "lizard.h"
#include <string.h>

namespace lizard {
static int clamp(int value, int limit) { return value < -limit ? -limit : value > limit ? limit : value; }
static int angle(int x, int y)
{
	// Diamond-angle approximation: continuous at the four axes, one turn
	// is 8192 units. No floating-point atan2 in the RF/USB service loop.
	int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
	int quarter = ay * 2048 / (ax + ay);
	return x >= 0 ? (y >= 0 ? quarter : 8192 - quarter) : (y >= 0 ? 4096 - quarter : 4096 + quarter);
}
void Mapper::reset() { *this = Mapper{}; }
void Mapper::observe(const Input &input, uint32_t now)
{
	if (seen_ && uint32_t(now - at_) > 250000) reset();
	input_ = input; at_ = now; seen_ = true;
	lt_ = (input.buttons & 2) || input.lt >= (lt_ ? 32 : 40);
	rt_ = (input.buttons & 1) || input.rt >= (rt_ ? 32 : 40);
	bool right = (input.buttons & (1u << 20)) != 0;
	if (right && right_touch_) {
		carry_x_ += int(input.rx) - rx_; carry_y_ -= int(input.ry) - ry_;
		x_ = clamp(x_ + carry_x_ / 128, 127); y_ = clamp(y_ + carry_y_ / 128, 127);
		carry_x_ %= 128; carry_y_ %= 128;
	}
	if (!right) carry_x_ = carry_y_ = 0;
	right_touch_ = right; rx_ = input.rx; ry_ = input.ry;
	if (!input.left_touch) { left_touch_ = false; carry_wheel_ = 0; }
	else if (input.left_sample) {
		int x = input.lpx, y = input.lpy;
		if (int64_t(x)*x + int64_t(y)*y < 6000*6000) { left_touch_ = false; carry_wheel_ = 0; }
		else {
			int current = angle(x, y);
			if (left_touch_ && uint32_t(now - left_at_) <= 200000) {
				int delta = current - angle_;
				if (delta > 4096) delta -= 8192;
				if (delta < -4096) delta += 8192;
				// Discontinuous jumps are a new anchor, not a large scroll.
				if (delta >= -2048 && delta <= 2048) {
					carry_wheel_ += delta * 24;
					wheel_ = clamp(wheel_ + carry_wheel_ / 8192, 15);
					carry_wheel_ %= 8192;
				} else carry_wheel_ = 0;
			} else carry_wheel_ = 0;
			angle_ = int16_t(current); left_at_ = now; left_touch_ = true;
		}
	}
}
Report Mapper::report(uint32_t now)
{
	Report out{};
	if (!seen_ || uint32_t(now - at_) > 250000) { reset(); return out; }
	uint32_t b = input_.buttons;
	out.buttons = uint8_t((rt_ || (b & (1u << 18)) ? 1 : 0) | (lt_ ? 2 : 0) | (b & (1u << 17) ? 4 : 0));
	out.x = int16_t(x_); out.y = int16_t(y_); out.wheel = int16_t(wheel_);
	unsigned count = 0;
	bool overflow = false;
	auto key = [&](uint8_t value) {
		for (unsigned i = 0; i < count; ++i) if (out.keyboard[2 + i] == value) return;
		if (count == 6) overflow = true;
		else out.keyboard[2 + count++] = value;
	};
	if (b & 8) out.keyboard[0] |= 1; // LB: left Ctrl
	if (b & 4) out.keyboard[0] |= 4; // RB: left Alt
	bool steam = (b & 0x2000) != 0;
	if (steam && (b & 0x8000)) { out.keyboard[0] |= 5; key(0x4c); }
	if (steam && (b & 0x40)) { out.keyboard[0] |= 9; key(0x12); }
	else if (b & 0x40) key(0x4b);
	if (b & 0x80) key(0x28);
	if (b & (0x20 | 0x4000)) key(0x29);
	if (b & 0x10) key(0x4e);
	if (b & 0x1000) key(0x2b);
	if (uint32_t(now - input_.stick_at) <= 200000) {
		if (input_.ly > 12000) key(0x52);
		if (input_.ly < -12000) key(0x51);
		if (input_.lx < -12000) key(0x50);
		if (input_.lx > 12000) key(0x4f);
	}
	if (overflow) memset(out.keyboard + 2, 1, 6); // standard six-key ErrorRollOver
	return out;
}
void Mapper::mouseSent(int x, int y, int wheel) { x_ -= x; y_ -= y; wheel_ -= wheel; }
}
