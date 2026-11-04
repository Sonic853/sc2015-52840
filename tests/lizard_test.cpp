#include "lizard.h"
#include "input_mode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "LIZARD FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
bool key(const lizard::Report &r, uint8_t code)
{
	for (unsigned i = 2; i < 8; ++i) if (r.keyboard[i] == code) return true;
	return false;
}
void put16(uint8_t *p, int v) { p[0] = uint8_t(v); p[1] = uint8_t(uint16_t(v) >> 8); }
void testBindings()
{
	lizard::Mapper mapper;
	const uint32_t buttons[] = {0x80,0x20,0x40,0x10,0x1000,0x4000};
	const uint8_t codes[] = {0x28,0x29,0x4b,0x4e,0x2b,0x29};
	for (unsigned i = 0; i < 6; ++i) {
		lizard::Input in{}; in.buttons = buttons[i]; mapper.observe(in, 1);
		auto r = mapper.report(1); CHECK(key(r,codes[i]) && r.keyboard[0] == 0 && r.buttons == 0);
		CHECK(r.keyboard[2] == codes[i] && r.keyboard[3] == 0);
	}
	lizard::Input in{}; in.buttons = 0x2000 | 0x40; mapper.observe(in,2);
	auto r = mapper.report(2); CHECK(r.keyboard[0] == 9 && key(r,0x12) && !key(r,0x4b));
	in.buttons = 0x2000 | 0x8000; mapper.observe(in,3);
	r = mapper.report(3); CHECK(r.keyboard[0] == 5 && key(r,0x4c));
	in.buttons = 8 | 4; mapper.observe(in,4); r = mapper.report(4);
	CHECK(r.keyboard[0] == 5 && r.keyboard[2] == 0);
	for (uint32_t b : {0x2000u,0x8000u,0x10000u,1u << 22}) {
		in.buttons = b; mapper.observe(in,5); r = mapper.report(5);
		for (uint8_t k : r.keyboard) CHECK(k == 0);
		CHECK(r.buttons == 0);
	}
	in.buttons = 1 | 2 | (1u << 17) | (1u << 18); mapper.observe(in,6); CHECK(mapper.report(6).buttons == 7);
	in.buttons = 0; in.rt = 39; mapper.observe(in,7); CHECK(mapper.report(7).buttons == 1); // release hysteresis
	in.rt = 31; mapper.observe(in,8); CHECK(mapper.report(8).buttons == 0);
	in.rt = 39; mapper.observe(in,9); CHECK(mapper.report(9).buttons == 0);
	in.rt = 40; in.lt = 40; mapper.observe(in,10); CHECK(mapper.report(10).buttons == 3);
	in.rt = in.lt = 0; in.buttons = 0x20 | 0x4000; mapper.observe(in,11);
	r = mapper.report(11); CHECK(r.keyboard[2] == 0x29 && r.keyboard[3] == 0);
	in.buttons = 0; in.lx = 12000; in.ly = -12000; in.stick_at = 12;
	mapper.observe(in,12); CHECK(mapper.report(12).keyboard[2] == 0);
	in.lx = 12001; in.ly = -12001; mapper.observe(in,13);
	r = mapper.report(13); CHECK(key(r,0x4f) && key(r,0x51));
	in.lx = -32768; in.ly = 32767; mapper.observe(in,14);
	r = mapper.report(14); CHECK(key(r,0x50) && key(r,0x52));
	CHECK(mapper.report(200013).keyboard[2] == 0); // stick coordinates expire separately
	in.buttons = 0x80 | 0x20 | 0x40 | 0x10 | 0x1000; in.stick_at = 15;
	mapper.observe(in,15); r = mapper.report(15);
	for (unsigned i = 2; i < 8; ++i) CHECK(r.keyboard[i] == 1);
	CHECK(mapper.report(250016).buttons == 0 && mapper.report(250016).keyboard[2] == 0);
}
void testPointerAndWheel()
{
	lizard::Mapper mapper; lizard::Input in{}; in.buttons = 1u << 20; in.rx = 10000;
	mapper.observe(in,100); CHECK(mapper.report(100).x == 0);
	in.rx += 64; mapper.observe(in,101); CHECK(mapper.report(101).x == 0);
	in.rx += 64; in.ry = 1280; mapper.observe(in,102);
	auto r = mapper.report(102); CHECK(r.x == 1 && r.y == -10);
	CHECK(mapper.report(103).x == 1); // failed/not attempted USB submission retains motion
	mapper.mouseSent(r.x,r.y,r.wheel); CHECK(mapper.report(103).x == 0 && mapper.report(103).y == 0);
	in.buttons = 0; mapper.observe(in,104); in.buttons = 1u << 20; in.rx = -30000;
	mapper.observe(in,105); CHECK(mapper.report(105).x == 0);
	in.rx = 32767; mapper.observe(in,106); CHECK(mapper.report(106).x == 127);
	in.rx = -32768; mapper.observe(in,250107); CHECK(mapper.report(250107).x == 0); // re-anchor after a gap
	mapper.reset(); in = {}; in.left_touch = in.left_sample = true;
	const int points[][2] = {{20000,0},{14142,14142},{0,20000},{-14142,14142},{-20000,0},{-14142,-14142},{0,-20000},{14142,-14142},{20000,0}};
	for (bool clockwise : {false,true}) {
		mapper.reset(); int total = 0;
		for (unsigned i = 0; i < 9; ++i) {
			in.lpx = int16_t(points[i][0]); in.lpy = int16_t(clockwise ? -points[i][1] : points[i][1]);
			mapper.observe(in,1000 + i*10000); r = mapper.report(1000 + i*10000);
			CHECK(r.wheel == (i ? (clockwise ? -3 : 3) : 0)); total += r.wheel;
			mapper.mouseSent(r.x,r.y,r.wheel);
		}
		CHECK(total == (clockwise ? -24 : 24));
	}
	mapper.reset(); in.lpx = 20000; in.lpy = 0; mapper.observe(in,0xfffffff0u);
	in.lpx = 0; in.lpy = 20000; mapper.observe(in,0x10); CHECK(mapper.report(0x10).wheel == 6);
	mapper.mouseSent(0,0,6); in.left_sample = false; in.lpx = -20000; in.lpy = 0;
	mapper.observe(in,0x20); CHECK(mapper.report(0x20).wheel == 0); // multiplexed stick packet
	in.left_sample = true; in.lpx = 20000; in.lpy = 0;
	mapper.observe(in,200017); CHECK(mapper.report(200017).wheel == 0); // old left-pad anchor expires
	in.lpx = 0; in.lpy = 0; mapper.observe(in,200018);
	in.lpx = -20000; mapper.observe(in,200019); CHECK(mapper.report(200019).wheel == 0); // center crossing
	in.lpx = 20000; mapper.observe(in,200020); CHECK(mapper.report(200020).wheel == 0); // >90 degree jump
	in.left_touch = false; mapper.observe(in,200021); in.left_touch = true; in.lpx = 0; in.lpy = 20000;
	mapper.observe(in,200022); CHECK(mapper.report(200022).wheel == 0);
}
void testRouting()
{
	using input_mode::Mode;
	uint8_t raw[64] = {1,0,1,60};
	for (auto start : {Mode::Steam,Mode::Xbox,Mode::PS5,Mode::Switch}) {
		input_mode::Router router; router.changeMode(start); raw[8] = 0; raw[9] = 0xa0; raw[10] = 1;
		for (unsigned i = 0; i < 4; ++i) CHECK(router.observe(0,1,true,raw,1000+i*100000));
		Mode target; CHECK(router.requested(target) && target == Mode::Lizard);
		router.changeMode(target); CHECK(router.lizardReport(301000).keyboard[2] == 0);
		raw[10] = 0; CHECK(router.observe(0,1,true,raw,401000)); // partial release remains suppressed
		raw[9] = 0; CHECK(!router.observe(0,1,true,raw,501000));
		raw[8] = 0x80; router.observe(0,1,true,raw,501001); CHECK(key(router.lizardReport(501001),0x28));
		router.observe(0,2,true,raw,501002); CHECK(key(router.lizardReport(501002),0x28));
		router.observe(0,2,false,raw,501003); CHECK(router.lizardReport(501003).keyboard[2] == 0);
	}
	for (uint32_t b : {0x8000u,0x10000u,0x18000u,0x1a008u}) {
		input_mode::Router router; raw[8] = uint8_t(b); raw[9] = uint8_t(b >> 8); raw[10] = uint8_t(b >> 16);
		for (unsigned i = 0; i < 4; ++i) router.observe(0,1,true,raw,i*100000);
		Mode target; CHECK(!router.requested(target));
	}
	input_mode::Router router; router.changeMode(Mode::Lizard);
	raw[8] = 0x80; raw[9] = 0xa0; raw[10] = 1;
	for (unsigned i = 0; i < 4; ++i) router.observe(0,1,true,raw,i*100000);
	Mode target; CHECK(router.requested(target) && target == Mode::Steam);
	router.reset(); router.changeMode(Mode::Lizard); memset(raw + 8,0,56);
	// Left-stick click aliases the pad click only when neither pad nor both-source flag is present.
	raw[10] = 2; router.observe(0,1,true,raw,1); CHECK(router.lizardReport(1).buttons == 0);
	raw[10] = 10; router.observe(0,1,true,raw,2); CHECK(router.lizardReport(2).buttons == 4);
	// Physical left stick and touched left pad remain separate on alternating reports.
	raw[10] = 0x80; put16(raw + 16,20000); router.observe(0,1,true,raw,3);
	CHECK(key(router.lizardReport(3),0x4f));
	raw[10] = 0x88; put16(raw + 16,-20000); router.observe(0,1,true,raw,4);
	CHECK(key(router.lizardReport(4),0x4f) && !key(router.lizardReport(4),0x50));
	router.clearMotion(); CHECK(router.lizardReport(4).keyboard[2] == 0);
	auto config = input_mode::defaults(); uint8_t record[48]; uint32_t generation;
	for (bool persist : {false,true}) {
		if (persist) config.flags |= input_mode::PERSIST_MODE;
		auto boot = input_mode::bootRequest(config,Mode::Lizard);
		input_mode::encodeRecord(config,19,record,boot);
		CHECK(input_mode::decodeRecord(record,config,generation,&boot) && generation == 19);
		CHECK(input_mode::consumeBoot(config,boot) == Mode::Lizard);
		CHECK(input_mode::consumeBoot(config,boot) == (persist ? Mode::Lizard : Mode::Steam));
	}
}
}
void testLizard()
{
	testBindings(); testPointerAndWheel(); testRouting();
	std::printf("PASS: Lizard keys, shortcuts, pointer, circular wheel, freshness and startup routing (%u assertions; software only)\n",checks);
}
