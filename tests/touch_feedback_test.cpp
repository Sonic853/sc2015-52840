#include "touch_feedback.h"
#include "input_mode.h"
#include "protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"TOUCH FAILED %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while (0)
void testTracker()
{
	touch_feedback::Tracker tracker;
	tracker.observe(true,10000,10000,1); CHECK(!tracker.due(1));
	for (unsigned i = 0; i < 100; ++i) {
		tracker.observe(true,int16_t(10000 + (i & 1 ? 32 : -32)),10000,1);
		CHECK(!tracker.due(1));
	}
	tracker.observe(true,10064,10000,2); CHECK(!tracker.due(2));
	tracker.observe(true,10128,10000,3); CHECK(tracker.due(3)); tracker.sent(3);
	tracker.observe(true,10256,10000,4); CHECK(!tracker.due(20002)); CHECK(tracker.due(20003)); tracker.sent(20003);
	tracker.observe(true,10256,10000,40003); CHECK(!tracker.due(40003));
	tracker.observe(true,10384,10000,40004); CHECK(tracker.due(40004));
	tracker.observe(false,0,0,40005); CHECK(!tracker.due(40005) && !tracker.active(40005));
	tracker.observe(true,-30000,-30000,40006); CHECK(!tracker.due(40006));
	tracker.observe(true,30000,30000,40007); CHECK(tracker.due(40007));
	CHECK(!tracker.due(140008));
	tracker.observe(true,0,0,140008); CHECK(!tracker.due(140008));
	tracker.reset(); tracker.observe(true,0,0,0xfffffff0u); tracker.observe(true,128,0,0xfffffff1u);
	CHECK(tracker.due(0xfffffff1u)); tracker.sent(0xfffffff1u);
	tracker.observe(true,256,0,0x10); CHECK(!tracker.due(0x10)); CHECK(tracker.due(0xfffffff1u + 20000u));
}
void testPulseAndConfig()
{
	uint8_t pulse[10]; touch_feedback::pulse(false,100,pulse);
	const uint8_t steam_right[] = {0x8f,8,0,100,0,0,0,1,0,0}; CHECK(!memcmp(pulse,steam_right,10));
	touch_feedback::pulse(true,533,pulse);
	const uint8_t steam_left[] = {0x8f,8,1,0x15,2,0,0,1,0,0}; CHECK(!memcmp(pulse,steam_left,10));
	auto config = input_mode::defaults(); CHECK(config.right_haptic_us == 100 && config.left_haptic_us == 533);
	uint8_t record[input_mode::RECORD_SIZE]; config.right_haptic_us = 0; config.left_haptic_us = 1000;
	input_mode::encodeRecord(config,71,record,{input_mode::Mode::Lizard,4});
	input_mode::Config read{}; input_mode::BootState boot; uint32_t generation;
	CHECK(input_mode::decodeRecord(record,read,generation,&boot));
	CHECK(generation == 71 && boot.once == 4 && read.right_haptic_us == 0 && read.left_haptic_us == 1000);
	config.right_haptic_us = 1001; CHECK(!input_mode::valid(config));
	// Old 48-byte v2 records default only the new fields; no out-of-bounds tail read.
	uint8_t old[48] = {}; memcpy(old,"SCXM",4); old[4] = 2;
	input_mode::encodeConfig(input_mode::defaults(),old + 12); old[36] = old[37] = 4;
	uint32_t crc = sc::crc32(old,40); memcpy(old + 40,&crc,4);
	CHECK(input_mode::decodeRecord(old,read,generation,&boot));
	CHECK(boot.once == 4 && read.right_haptic_us == 100 && read.left_haptic_us == 533);
	uint8_t wire[24]; input_mode::encodeConfig(read,wire);
	CHECK(input_mode::decodeConfig(wire,read) && read.right_haptic_us == 100);
}
void testRouting()
{
	using input_mode::Mode;
	for (auto mode : {Mode::Steam,Mode::Xbox,Mode::PS5,Mode::Switch,Mode::Lizard}) {
		input_mode::Router router; router.changeMode(mode); uint8_t raw[64] = {1,0,1,60}; raw[10] = 0x10;
		router.observe(0,1,true,raw,1000); raw[20] = 128; router.observe(0,1,true,raw,2000);
		CHECK(router.rightTouchDue(2000) == (mode == Mode::Xbox || mode == Mode::PS5 || mode == Mode::Switch || mode == Mode::Lizard));
		router.clearMotion(); CHECK(!router.rightTouchDue(2000));
		router.observe(0,1,true,raw,3000); raw[21] = 1; router.observe(0,1,true,raw,4000);
		router.observe(0,2,true,raw,5000); CHECK(!router.rightTouchDue(5000));
		router.observe(0,2,false,raw,6000); CHECK(!router.rightTouchActive(6000));
	}
}
}
void testTouchFeedback()
{
	testTracker(); testPulseAndConfig(); testRouting();
	std::printf("PASS: touch feedback motion gates, Steam pulse fixtures, config migration and owner resets (%u assertions)\n",checks);
}
