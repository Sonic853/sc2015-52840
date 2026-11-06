#pragma once
#include <stdint.h>

namespace right_stick {
struct Settings {
	uint8_t mode; // 0: fixed center, 1: swipe velocity
	uint16_t exponent, full_speed, smoothing_ms, anti_deadzone, noise_speed;
};
Settings defaults();
bool valid(const Settings &settings);
struct Axes { int16_t x, y; };
class Mapper {
public:
	void reset();
	void observe(bool touched, int16_t x, int16_t y, uint32_t sampled_at,
		     uint32_t now, const Settings &settings);
	Axes axes(uint32_t now, const Settings &settings) const;
private:
	bool touched_ = false;
	int16_t x_ = 0, y_ = 0;
	int32_t dx_ = 0, dy_ = 0;
	uint32_t at_ = 0, window_at_ = 0, motion_at_ = 0;
	float vx_ = 0, vy_ = 0;
};
}
