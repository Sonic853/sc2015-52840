#pragma once
#include <stdint.h>

namespace touch_feedback {
constexpr uint16_t DEFAULT_RIGHT_US = 100, DEFAULT_LEFT_US = 533, MAX_US = 1000;
constexpr uint32_t INTERVAL_US = 20000, MAX_AGE_US = 100000;
void pulse(bool left, uint16_t on_us, uint8_t out[10]);
class Tracker {
public:
	void reset();
	void observe(bool touched, int16_t x, int16_t y, uint32_t now);
	bool active(uint32_t now) const;
	bool due(uint32_t now) const;
	void sent(uint32_t now);
private:
	bool touched_ = false, pending_ = false, sent_ = false;
	int16_t x_ = 0, y_ = 0;
	uint32_t at_ = 0, event_at_ = 0, sent_at_ = 0;
};
}
