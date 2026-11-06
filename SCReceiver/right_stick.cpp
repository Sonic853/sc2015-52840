#include "right_stick.h"
#include <math.h>

namespace right_stick {
static constexpr uint32_t TIMEOUT_US = 50000, WINDOW_US = 4000;
Settings defaults() { return {0,100,200,10,0,2}; }
bool valid(const Settings &s)
{
	return s.mode <= 1 && s.exponent >= 50 && s.exponent <= 300 &&
		s.full_speed >= 20 && s.full_speed <= 2000 && s.smoothing_ms <= 40 &&
		s.anti_deadzone <= 500 && s.noise_speed <= 50 && s.noise_speed < s.full_speed;
}
void Mapper::reset() { *this = Mapper{}; }
void Mapper::observe(bool touched, int16_t x, int16_t y, uint32_t sampled_at,
		     uint32_t now, const Settings &s)
{
	if (!touched || uint32_t(now - sampled_at) > TIMEOUT_US) { reset(); return; }
	if (!touched_ || uint32_t(sampled_at - at_) > TIMEOUT_US) {
		reset(); touched_ = true; x_ = x; y_ = y;
		at_ = window_at_ = motion_at_ = sampled_at; return;
	}
	// Equal timestamps are not a second time interval. Keep the previous
	// anchor so the next timed sample accounts for the total displacement.
	if (sampled_at == at_) return;
	dx_ += int32_t(x) - x_; dy_ += int32_t(y) - y_;
	x_ = x; y_ = y; at_ = sampled_at;
	uint32_t dt = sampled_at - window_at_;
	if (dt < WINDOW_US) return;
	float vx = float(dx_) * 1000.0f / dt, vy = float(dy_) * 1000.0f / dt;
	if (uint32_t(sampled_at - motion_at_) > TIMEOUT_US) vx_ = vy_ = 0;
	if (vx*vx + vy*vy > float(s.noise_speed)*s.noise_speed) motion_at_ = sampled_at;
	float alpha = s.smoothing_ms ? 1.0f - expf(-float(dt) / (1000.0f*s.smoothing_ms)) : 1.0f;
	vx_ += alpha * (vx - vx_); vy_ += alpha * (vy - vy_);
	dx_ = dy_ = 0; window_at_ = sampled_at;
}
Axes Mapper::axes(uint32_t now, const Settings &s) const
{
	if (!touched_ || uint32_t(now - at_) > TIMEOUT_US || uint32_t(now - motion_at_) > TIMEOUT_US) return {};
	float speed = sqrtf(vx_*vx_ + vy_*vy_);
	if (speed <= s.noise_speed || speed < 0.01f) return {};
	float r = (speed - s.noise_speed) / (s.full_speed - s.noise_speed);
	if (r > 1) r = 1;
	float a = s.anti_deadzone / 1000.0f;
	float magnitude = (a + (1-a)*powf(r,s.exponent/100.0f)) * 32767.0f / speed;
	return {int16_t(lroundf(vx_*magnitude)), int16_t(lroundf(vy_*magnitude))};
}
}
