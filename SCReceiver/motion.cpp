#include "motion.h"
#include <string.h>

namespace motion {
static int16_t get16(const uint8_t *p) { return int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }
static int16_t scale(int32_t value, int32_t numerator, int32_t denominator)
{
	int64_t product = int64_t(value) * numerator;
	int64_t rounded = product < 0 ? -((-product + denominator / 2) / denominator) : (product + denominator / 2) / denominator;
	return int16_t(rounded < -32768 ? -32768 : rounded > 32767 ? 32767 : rounded);
}
void Tracker::reset() { sample_ = {}; }
void Tracker::observe(const uint8_t report[64], uint8_t updated, uint32_t now)
{
	if (updated & Clear) reset();
	if (report[0] != 1 || report[1] || report[2] != 1 || report[3] != 60) return;
	if (updated & Accel) {
		for (unsigned i = 0; i < 3; ++i) sample_.accel[i] = get16(report + 28 + 2 * i);
		sample_.accel_at = now; sample_.valid |= Accel;
	}
	if (updated & Gyro) {
		for (unsigned i = 0; i < 3; ++i) sample_.gyro[i] = get16(report + 34 + 2 * i);
		sample_.gyro_at = now; sample_.valid |= Gyro;
	}
}
Sample Tracker::read(uint32_t now) const
{
	Sample out = sample_;
	if (!(out.valid & Accel) || uint32_t(now - out.accel_at) > MAX_AGE_US) {
		out.valid &= uint8_t(~Accel); memset(out.accel, 0, sizeof(out.accel));
	}
	if (!(out.valid & Gyro) || uint32_t(now - out.gyro_at) > MAX_AGE_US) {
		out.valid &= uint8_t(~Gyro); memset(out.gyro, 0, sizeof(out.gyro));
	}
	return out;
}
Axes dualsense(const Sample &s)
{
	Axes out{};
	if (s.valid & Gyro) {
		out.gyro[0] = s.gyro[0]; out.gyro[1] = s.gyro[2]; out.gyro[2] = s.gyro[1];
	}
	if (s.valid & Accel) {
		out.accel[0] = scale(s.accel[0],1,2); out.accel[1] = scale(s.accel[2],1,2);
		out.accel[2] = scale(-int32_t(s.accel[1]),1,2);
	}
	return out;
}
Axes switchPro(const Sample &s)
{
	Axes out{};
	if (s.valid & Gyro) {
		// SDL Switch raw rate = 936 / 13371 deg/s; factory calibration is
		// 13371. Convert the SC2015 2000/32768 rate exactly before rounding.
		out.gyro[0] = scale(-int32_t(s.gyro[1]),557125,638976);
		out.gyro[1] = scale(-int32_t(s.gyro[0]),557125,638976);
		out.gyro[2] = scale(s.gyro[2],557125,638976);
	}
	if (s.valid & Accel) {
		out.accel[0] = scale(s.accel[1],1,4); out.accel[1] = scale(-int32_t(s.accel[0]),1,4);
		out.accel[2] = scale(s.accel[2],1,4);
	}
	return out;
}
}
