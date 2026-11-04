#pragma once
#include <stdint.h>

namespace motion {
enum : uint8_t { Accel = 1, Gyro = 2, Clear = 4 };
constexpr uint32_t MAX_AGE_US = 100000;
struct Sample {
	int16_t accel[3], gyro[3];
	uint32_t accel_at, gyro_at;
	uint8_t valid;
};
struct Axes { int16_t accel[3], gyro[3]; };
class Tracker {
public:
	void reset();
	void observe(const uint8_t report[64], uint8_t updated, uint32_t now);
	Sample read(uint32_t now) const;
private:
	Sample sample_{};
};
// Source: SC2015, 2g/32768 and 2000deg/s/32768. These are deliberately not
// OpenPuck's SC2 gyro signs or its device-specific sensitivity trims.
Axes dualsense(const Sample &sample);
Axes switchPro(const Sample &sample);
}
