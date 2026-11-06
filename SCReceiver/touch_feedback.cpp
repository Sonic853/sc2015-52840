#include "touch_feedback.h"
#include <string.h>

namespace touch_feedback {
void pulse(bool left, uint16_t on_us, uint8_t out[10])
{
	// Same single-pulse shape as the captured Steam trackpad feedback:
	// right 8f080064000000010000; left 8f080115020000010000.
	memset(out, 0, 10); out[0] = 0x8f; out[1] = 8; out[2] = left ? 1 : 0;
	out[3] = uint8_t(on_us); out[4] = uint8_t(on_us >> 8); out[7] = on_us ? 1 : 0;
}
void Tracker::reset() { *this = Tracker{}; }
bool Tracker::active(uint32_t now) const { return touched_ && uint32_t(now - at_) <= MAX_AGE_US; }
void Tracker::observe(bool touched, int16_t x, int16_t y, uint32_t now)
{
	if (!active(now) || !touched) { pending_ = false; touched_ = false; }
	if (touched && touched_) {
		int dx = int(x) - x_, dy = int(y) - y_;
		// Measure from the last event anchor, not the sum of absolute jitter.
		// Small oscillations while a finger is still must not accumulate ticks.
		unsigned distance = unsigned(dx < 0 ? -dx : dx) + unsigned(dy < 0 ? -dy : dy);
		if (distance >= 128) { pending_ = true; event_at_ = now; x_ = x; y_ = y; }
	} else { x_ = x; y_ = y; }
	touched_ = touched; at_ = now;
}
bool Tracker::due(uint32_t now) const
{
	return active(now) && pending_ && uint32_t(now - event_at_) <= MAX_AGE_US &&
		(!sent_ || uint32_t(now - sent_at_) >= INTERVAL_US);
}
void Tracker::sent(uint32_t now) { pending_ = false; sent_ = true; sent_at_ = now; }
}
