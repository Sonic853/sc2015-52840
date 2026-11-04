#include "motion.h"
#include "input_mode.h"
#include "ps5.h"
#include "switch_pro.h"
#include <Adafruit_TinyUSB.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
unsigned assertions;
#define CHECK(value) do { ++assertions; if (!(value)) { \
	std::fprintf(stderr, "MOTION FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); \
} } while (0)
int16_t get16(const uint8_t *p) { return int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }
void put16(uint8_t *p, int16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(uint16_t(value) >> 8); }
void rawSample(uint8_t raw[64])
{
	memset(raw, 0, 64); raw[0] = raw[2] = 1; raw[3] = 60;
	put16(raw + 28, 16384); put16(raw + 32, -8192);
	put16(raw + 34, 1000); put16(raw + 36, -2000); put16(raw + 38, 3000);
}
void testFreshness()
{
	uint8_t raw[64]; rawSample(raw); motion::Tracker tracker;
	CHECK(tracker.read(0).valid == 0);
	tracker.observe(raw, motion::Accel, 0);
	tracker.observe(raw, motion::Gyro, 50000);
	auto sample = tracker.read(100000);
	CHECK(sample.valid == 3 && sample.accel[0] == 16384 && sample.gyro[1] == -2000);
	CHECK(sample.accel_at == 0 && sample.gyro_at == 50000);
	// A reconstructed report still contains old sensor bytes. Buttons cannot
	// make them fresh, even when the report's sequence and input time advance.
	tracker.observe(raw, 0, 100001); sample = tracker.read(100001);
	CHECK(sample.valid == motion::Gyro && sample.accel[0] == 0 && sample.gyro[1] == -2000);
	CHECK(tracker.read(150000).valid == motion::Gyro && tracker.read(150001).valid == 0);
	raw[1] = 1; tracker.observe(raw, 3, 200000); CHECK(tracker.read(200000).valid == 0); raw[1] = 0;
	tracker.observe(raw, 3, 200000); tracker.observe(raw, motion::Clear | motion::Gyro, 200001);
	sample = tracker.read(200001); CHECK(sample.valid == motion::Gyro && sample.accel[0] == 0);
	tracker.observe(raw, motion::Clear, 200002); CHECK(tracker.read(200002).valid == 0);
	tracker.observe(raw, 3, 0xfffffff0u);
	CHECK(tracker.read(0x10).valid == 3);
	CHECK(tracker.read(0xfffffff0u + 100001u).valid == 0);
	tracker.reset(); CHECK(tracker.read(0).valid == 0);
}
void testHostUnits()
{
	ps5::enable(true, 1, 2);
	uint8_t calibration[40], mapped[20] = {0,20}, report[63];
	CHECK(ps5::getReport(5, HID_REPORT_TYPE_FEATURE, calibration, 40) == 40);
	const double speed = get16(calibration + 18) + get16(calibration + 20);
	CHECK(speed > 0);
	TinyUSBDevice.mounted_state = true; TinyUSBDevice.suspended_state = false;
	switch_pro::enable(true, 1, 2);
	uint8_t spi_request[15] = {}; spi_request[9] = 0x10; spi_request[10] = 0x20; spi_request[11] = 0x60; spi_request[14] = 24;
	switch_pro::setReport(1, HID_REPORT_TYPE_OUTPUT, spi_request, sizeof(spi_request));
	switch_pro::Report spi;
	CHECK(switch_pro::nextReport(mapped, 100000, spi) && spi.id == 0x21 && spi.data[12] == 0x90 && spi.data[18] == 24);
	double pro_gyro_scale[3], pro_accel_scale[3];
	for (unsigned i = 0; i < 3; ++i) {
		const uint8_t *p = spi.data + 19 + 2*i;
		CHECK(get16(p) == 0 && get16(p + 12) == 0);
		CHECK(get16(p + 6) == 16384 && get16(p + 18) == 13371);
		pro_accel_scale[i] = 4.0 / (get16(p + 6) - get16(p));
		pro_gyro_scale[i] = 936.0 / (get16(p + 18) - get16(p + 12));
	}
	switch_pro::enable(false, 0, 0);
	// Independent oracle: compare host-decoded physical units to SDL's
	// SC2015 coordinate system, including every signed extreme and axis.
	const int16_t values[] = {-32768,-16384,-100,-1,0,1,100,16384,32767};
	for (int16_t value : values) for (unsigned source_axis = 0; source_axis < 3; ++source_axis) {
		motion::Sample s{}; s.valid = 3; s.accel[source_axis] = s.gyro[source_axis] = value;
		const double gyro[] = {s.gyro[0] * 2000.0 / 32768, s.gyro[2] * 2000.0 / 32768, s.gyro[1] * 2000.0 / 32768};
		const double accel[] = {s.accel[0] / 16384.0, s.accel[2] / 16384.0, -s.accel[1] / 16384.0};
		ps5::buildReport(mapped, 1234, report, &s);
		for (unsigned i = 0; i < 3; ++i) {
			double gyro_range = get16(calibration + 6 + 4*i) - get16(calibration + 8 + 4*i);
			double accel_plus = get16(calibration + 22 + 4*i), accel_minus = get16(calibration + 24 + 4*i);
			CHECK(gyro_range > 0 && accel_plus > accel_minus);
			CHECK(std::fabs(get16(report + 15 + 2*i) * speed / gyro_range - gyro[i]) < 0.00001);
			CHECK(std::fabs((get16(report + 21 + 2*i) - (accel_plus + accel_minus)/2) * 2 / (accel_plus - accel_minus) - accel[i]) <= 1.0/16384);
		}
		auto pro = motion::switchPro(s);
		const double pro_gyro[] = {-pro.gyro[1] * pro_gyro_scale[1], pro.gyro[2] * pro_gyro_scale[2], -pro.gyro[0] * pro_gyro_scale[0]};
		const double pro_accel[] = {-pro.accel[1] * pro_accel_scale[1], pro.accel[2] * pro_accel_scale[2], -pro.accel[0] * pro_accel_scale[0]};
		for (unsigned i = 0; i < 3; ++i) {
			CHECK(std::fabs(pro_gyro[i] - gyro[i]) <= 0.5 * 936/13371);
			CHECK(std::fabs(pro_accel[i] - accel[i]) <= 0.5/4096);
		}
	}
	motion::Sample absent{}; absent.gyro[0] = absent.accel[0] = 1234;
	ps5::buildReport(mapped, 1235, report, &absent);
	for (unsigned i = 15; i < 27; ++i) CHECK(report[i] == 0);
	auto pro = motion::switchPro(absent); CHECK(pro.gyro[1] == 0 && pro.accel[1] == 0);
	ps5::enable(false, 0, 0);
}
void testRouterIsolation()
{
	using input_mode::Mode;
	uint8_t raw[64]; rawSample(raw); input_mode::Router router;
	router.changeMode(Mode::PS5);
	router.observe(0, 1, true, raw, 0, false, 3); CHECK(router.motionSample(0).valid == 3);
	router.observe(1, 1, true, raw, 50000, false, motion::Gyro);
	CHECK(router.owner() == 0 && router.motionSample(50000).gyro_at == 0);
	router.observe(0, 2, true, raw, 50001, false, 0); CHECK(router.motionSample(50001).valid == 0);
	router.observe(0, 2, true, raw, 50002, false, 3);
	router.observe(0, 2, false, raw, 50003, false, 0); CHECK(router.motionSample(50003).valid == 0);
	router.observe(1, 1, true, raw, 50004, false, 0);
	CHECK(router.owner() == 1 && router.motionSample(50004).valid == motion::Gyro);
	router.changeMode(Mode::Switch); CHECK(router.motionSample(50004).valid == 0);
	router.observe(1, 1, true, raw, 50005, false, 3);
	router.configure(router.config()); CHECK(router.motionSample(50005).valid == 0);
	router.observe(1, 1, true, raw, 50006, false, 3);
	router.unplug(); router.changeMode(Mode::PS5);
	router.observe(1, 1, true, raw, 50007, false, 0); CHECK(router.motionSample(50007).valid == 0);
	router.reset(); raw[8] = 8; raw[9] = 0x20;
	router.observe(0, 3, true, raw, 0, true, 3);
	for (uint32_t at : {100000u,200000u,300000u}) router.observe(0, 3, true, raw, at, true, 3);
	Mode mode; CHECK(router.requested(mode) && mode == Mode::Switch);
	router.changeMode(mode); CHECK(router.motionSample(300000).valid == 0);
	raw[8] = raw[9] = 0; router.observe(0, 3, true, raw, 300001, true, 0);
	CHECK(router.motionSample(300001).valid == 0);
}
void testSwitchPackets()
{
	TinyUSBDevice.mounted_state = true; TinyUSBDevice.suspended_state = false;
	switch_pro::enable(true, 1, 2);
	uint8_t mapped[20] = {0,20}, raw[64]; rawSample(raw);
	motion::Tracker tracker; tracker.observe(raw, 3, 100000); auto sample = tracker.read(100000);
	switch_pro::Report reply;
	auto command = [&](uint8_t id, uint8_t value) {
		uint8_t p[11] = {}; p[9] = id; p[10] = value;
		switch_pro::setReport(1, HID_REPORT_TYPE_OUTPUT, p, sizeof(p));
		CHECK(switch_pro::nextReport(mapped, 100000, reply, &sample) && reply.id == 0x21);
		switch_pro::sent(reply, 100000);
	};
	command(3, 0x30); command(0x40, 1);
	CHECK(switch_pro::nextReport(mapped, 100000, reply, &sample) && reply.id == 0x30);
	for (unsigned i = 0; i < 3; ++i) {
		const uint8_t *p = reply.data + 12 + 12*i;
		CHECK(get16(p) == 0 && get16(p + 2) == -4096 && get16(p + 4) == -2048);
		CHECK(get16(p + 6) == 1744 && get16(p + 8) == -872 && get16(p + 10) == 2616);
	}
	switch_pro::sent(reply, 100000);
	CHECK(!switch_pro::nextReport(mapped, 114999, reply, &sample));
	CHECK(switch_pro::nextReport(mapped, 115000, reply, &sample)); switch_pro::sent(reply, 115000);
	sample = tracker.read(200001);
	CHECK(switch_pro::nextReport(mapped, 200001, reply, &sample));
	for (unsigned i = 12; i < 48; ++i) CHECK(reply.data[i] == 0);
	switch_pro::sent(reply, 200001); command(0x40, 0);
	CHECK(!switch_pro::nextReport(mapped, 204000, reply, &sample));
	CHECK(switch_pro::nextReport(mapped, 204001, reply, &sample));
	switch_pro::enable(false, 0, 0);
}
}
void testMotion()
{
	testFreshness(); testHostUnits(); testRouterIsolation(); testSwitchPackets();
	std::printf("PASS: SC2015 IMU freshness, host-unit oracle, owner isolation and Switch sample cadence (%u assertions; software only)\n", assertions);
}
