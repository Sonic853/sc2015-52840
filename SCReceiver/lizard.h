#pragma once
#include <stdint.h>

namespace lizard {
struct Input {
	uint32_t buttons, stick_at;
	int16_t lx, ly, lpx, lpy, rx, ry;
	uint8_t lt, rt;
	bool left_sample, left_touch;
};
struct Report {
	uint8_t keyboard[8], buttons;
	int16_t x, y, wheel;
};
class Mapper {
public:
	void reset();
	void observe(const Input &input, uint32_t now);
	Report report(uint32_t now);
	void mouseSent(int x, int y, int wheel);
private:
	Input input_{};
	uint32_t at_ = 0, left_at_ = 0;
	bool seen_ = false, right_touch_ = false, left_touch_ = false, lt_ = false, rt_ = false;
	int16_t rx_ = 0, ry_ = 0, angle_ = 0;
	int carry_x_ = 0, carry_y_ = 0, carry_wheel_ = 0;
	int x_ = 0, y_ = 0, wheel_ = 0;
};
}
