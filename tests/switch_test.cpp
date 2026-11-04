#include "switch_pro.h"
#include "input_mode.h"
#include "protocol.h"
#include "usb.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <device/dcd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { std::fprintf(stderr, "SWITCH FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
uint8_t neutral[20] = {0,20};
void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8*i)); }
void put16(uint8_t *p, int v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
unsigned x12(const uint8_t *p) { return unsigned(p[0]) | (unsigned(p[1] & 15) << 8); }
unsigned y12(const uint8_t *p) { return unsigned(p[1] >> 4) | (unsigned(p[2]) << 4); }
void sub(uint8_t id, const uint8_t *args = nullptr, unsigned n = 0)
{
	uint8_t p[63] = {}; p[2] = p[6] = 1; p[3] = p[4] = p[7] = p[8] = 0x40; p[9] = id;
	if (n) memcpy(p + 10, args, n);
	switch_pro::setReport(1, HID_REPORT_TYPE_OUTPUT, p, uint16_t(10 + n));
}
switch_pro::Report reply(uint8_t id)
{
	switch_pro::Report r; CHECK(switch_pro::nextReport(neutral, mock_micros, r)); CHECK(r.id == id);
	switch_pro::sent(r, mock_micros); return r;
}
void initialize()
{
	TinyUSBDevice.mounted_state = true; TinyUSBDevice.suspended_state = false; mock_micros = 100000;
	switch_pro::enable(true, 0x12345678, 0xabcdef01);
}
void testProtocol()
{
	initialize(); switch_pro::Report r, again;
	CHECK(!switch_pro::nextReport(neutral, mock_micros, r));
	for (uint8_t cmd : {uint8_t(1),uint8_t(2),uint8_t(3)}) {
		switch_pro::setReport(0x80, HID_REPORT_TYPE_OUTPUT, &cmd, 1);
		CHECK(switch_pro::nextReport(neutral, mock_micros, r) && r.id == 0x81 && r.data[0] == cmd);
		CHECK(switch_pro::nextReport(neutral, mock_micros, again) && again.serial == r.serial); // failed send retains head
		if (cmd == 1) CHECK(r.data[2] == 3 && r.data[3] == 2);
		for (unsigned i = cmd == 1 ? 9 : 1; i < 63; ++i) CHECK(r.data[i] == 0);
		switch_pro::sent(r, mock_micros);
	}
	sub(2); r = reply(0x21); CHECK(r.data[12] == 0x82 && r.data[13] == 2 && r.data[16] == 3);
	uint8_t mac[6]; memcpy(mac, r.data + 18, 6);
	for (uint8_t stage : {uint8_t(1),uint8_t(2),uint8_t(3)}) {
		sub(1, &stage, 1); r = reply(0x21); CHECK(r.data[12] == 0x81 && r.data[14] == stage);
		if (stage == 1) CHECK(!memcmp(r.data + 15, mac, 6) && !memcmp(r.data + 24, "Pro Controller", 14));
	}
	uint8_t args[34] = {}; put32(args, 0x603d); args[4] = 18;
	sub(0x10, args, 5); r = reply(0x21);
	CHECK(r.data[12] == 0x90 && r.data[18] == 18 && !memcmp(r.data + 14, args, 5));
	CHECK(x12(r.data + 19) == 1800 && y12(r.data + 19) == 1800);
	CHECK(x12(r.data + 22) == 2048 && y12(r.data + 22) == 2048);
	CHECK(x12(r.data + 28) == 2048 && y12(r.data + 28) == 2048);
	put32(args, 0x8000); args[4] = 2; args[5] = 0xb2; args[6] = 0xa1;
	sub(0x11, args, 7); CHECK(reply(0x21).data[12] == 0x80);
	sub(0x10, args, 5); r = reply(0x21); CHECK(r.data[19] == 0xb2 && r.data[20] == 0xa1);
	switch_pro::reset(); sub(0x10, args, 5); CHECK(reply(0x21).data[19] == 0xb2); // RAM calibration survives bus reset
	put32(args, 0x80ff); sub(0x11, args, 7); CHECK(reply(0x21).data[12] == 0);
	put32(args, 0xe9000); sub(0x11, args, 7); CHECK(reply(0x21).data[12] == 0); // no receiver Flash access
	put32(args, 0xfffffff0); args[4] = 255; sub(0x10, args, 5); r = reply(0x21);
	CHECK(r.data[18] == 29); for (unsigned i = 19; i < 48; ++i) CHECK(r.data[i] == 255);
	sub(0xee); CHECK(reply(0x21).data[12] == 0);
	uint8_t mode = 0x3f; sub(3, &mode, 1); CHECK(reply(0x21).data[12] == 0);
	CHECK(!switch_pro::nextReport(neutral, mock_micros, r));
	// Overflow drops the entire request, not just its reply/state pairing.
	uint8_t cmd = 2; for (unsigned i = 0; i < 8; ++i) switch_pro::setReport(0x80, HID_REPORT_TYPE_OUTPUT, &cmd, 1);
	mode = 0x30; sub(3, &mode, 1);
	for (unsigned i = 0; i < 8; ++i) CHECK(reply(0x81).data[0] == 2);
	CHECK(!switch_pro::nextReport(neutral, mock_micros, r));
	sub(3, &mode, 1); CHECK(reply(0x21).data[12] == 0x80);
	CHECK(switch_pro::nextReport(neutral, mock_micros, r) && r.id == 0x30);
	switch_pro::sent(r, mock_micros); CHECK(!switch_pro::nextReport(neutral, mock_micros + 3999, again));
	CHECK(switch_pro::nextReport(neutral, mock_micros + 4000, again));
	switch_pro::reset(); switch_pro::setReport(0x80, HID_REPORT_TYPE_OUTPUT, &cmd, 1);
	switch_pro::sent(r, mock_micros); CHECK(reply(0x81).data[0] == 2); // old ticket cannot erase a new reply
	uint8_t lights = 0x53; sub(0x30, &lights, 1); reply(0x21); sub(0x31); r = reply(0x21);
	CHECK(r.data[12] == 0xb0 && r.data[14] == lights);
	sub(0x21); CHECK(reply(0x21).data[12] == 0xa0);
}
void testMapping()
{
	initialize(); uint8_t mode = 0x30; sub(3, &mode, 1); reply(0x21);
	uint8_t mapped[20] = {0,20}, buffer[65]; switch_pro::Report r;
	CHECK(switch_pro::nextReport(mapped, mock_micros, r));
	CHECK(x12(r.data + 5) == 2048 && y12(r.data + 5) == 2048 && r.data[2] == 0 && r.data[3] == 0 && r.data[4] == 0);
	put16(mapped + 2, 0xf7f0); mapped[4] = 39; mapped[5] = 40;
	put16(mapped + 6, -32768); put16(mapped + 8, 32767); put16(mapped + 10, 32767); put16(mapped + 12, -32768);
	CHECK(switch_pro::nextReport(mapped, mock_micros, r));
	CHECK(r.data[2] == 0xcf && r.data[3] == 0x1f && r.data[4] == 0x40);
	CHECK(x12(r.data + 5) == 248 && y12(r.data + 5) == 3848 && x12(r.data + 8) == 3848 && y12(r.data + 8) == 248);
	for (unsigned bits = 0; bits < 16; ++bits) {
		put16(mapped + 2, int(bits)); mapped[4] = mapped[5] = 0;
		CHECK(switch_pro::nextReport(mapped, mock_micros, r));
		CHECK(r.data[4] == (((bits & 2) ? 1 : 0) | ((bits & 1) ? 2 : 0) | ((bits & 8) ? 4 : 0) | ((bits & 4) ? 8 : 0)));
	}
	uint8_t on = 1; sub(0x40, &on, 1); reply(0x21); CHECK(switch_pro::nextReport(neutral, mock_micros, r));
	CHECK(r.data[17] == 0 && r.data[29] == 0 && r.data[41] == 0); // no synthetic gravity without a source sample
	memset(buffer, 0xcd, sizeof(buffer)); CHECK(switch_pro::getReport(0x30, HID_REPORT_TYPE_INPUT, buffer + 1, 63) == 63);
	CHECK(buffer[0] == 0xcd && buffer[64] == 0xcd);
	CHECK(!switch_pro::getReport(0xf9, HID_REPORT_TYPE_FEATURE, buffer, 64));
}
void testRumble()
{
	initialize(); uint8_t mode = 0x30, on = 1; sub(3, &mode, 1); reply(0x21); sub(0x48, &on, 1); reply(0x21);
	rumble::Command command; CHECK(switch_pro::setRumbleTarget(0, 17));
	auto send = [&](uint32_t left, uint32_t right = 0x40400100) {
		uint8_t out[10] = {0x10}; put32(out + 2, left); put32(out + 6, right);
		switch_pro::setReport(0, HID_REPORT_TYPE_OUTPUT, out, sizeof(out)); CHECK(switch_pro::takeRumble(command));
		CHECK(command.slot == 0 && command.session == 17);
	};
	auto absolute = [](unsigned c) { return 0x40000100u | (c << 23) | (c << 9); };
	auto oracle = [](unsigned c) { int u = !c ? -256 : c < 16 ? int(c)*8-248 : c < 32 ? int(c)*2-158 : int(c)-127;
		return u <= -255 ? 0 : int(std::round(255.0 * std::exp2(u / 32.0))); };
	for (unsigned c = 0; c < 128; ++c) {
		send(absolute(c), absolute(127-c));
		CHECK(std::abs(int(command.low) - oracle(c)) <= 1 && std::abs(int(command.high) - oracle(127-c)) <= 1);
	}
	send(0,0); CHECK(command.low == 255 && command.high == 0); // mode zero holds
	send(0x40400100); CHECK(command.low == 0); // canonical neutral is silence
	send(0x40000000 | (1u << 25)); CHECK(command.low == 255); // single 5-bit preset
	send(0x80000000 | (1u << 25)); CHECK(command.low == 255); send(0); CHECK(command.low == 0); // peak then off
	send(0xc0000000 | (1u << 15)); CHECK(command.low == 255); send(0); CHECK(command.low == 0);
	send(0x40000000 | (127u << 23) | 1); CHECK(command.low == 255); send(0); CHECK(command.low == 0);
	send(0x40000000 | (127u << 23) | 5); CHECK(command.low == 0); // frequency-only 7-bit field
	send(0x80000000 | (127u << 23) | 1); CHECK(command.low == 255); send(0); CHECK(command.low == 0);
	send(0x40000000 | (29u << 25) | (29u << 20)); CHECK(command.low == 0); // clamp below silence
	send(absolute(127)); uint32_t token = command.usb_generation;
	switch_pro::stopRumble(); CHECK(!switch_pro::takeRumble(command)); CHECK(switch_pro::setRumbleTarget(0, 17) != token);
	send(0); CHECK(command.low == 0); // old hold state cannot follow a USB/RF target change
	on = 0; sub(0x48, &on, 1); reply(0x21); CHECK(switch_pro::setRumbleTarget(0, 17) == 0);
}
bool aes(const uint8_t *, const uint8_t *, uint8_t *) { return false; }
bool random(uint8_t *p, size_t n) { memset(p, 7, n); return true; }
bool persist(const sc::Database &) { return true; }
void testChordsAndUsb()
{
	uint8_t raw[64] = {1,0,1,60};
	for (auto initial : {input_mode::Mode::Steam,input_mode::Mode::Xbox,input_mode::Mode::PS5}) {
		for (uint32_t buttons : {8u,0x2000u,0x2008u,0xa008u,0x12008u}) {
			input_mode::Router router; router.changeMode(initial);
			raw[8] = uint8_t(buttons); raw[9] = uint8_t(buttons >> 8); raw[10] = uint8_t(buttons >> 16);
			for (unsigned i = 0; i < 4; ++i) router.observe(0, 1, true, raw, 1000 + i*100000);
			input_mode::Mode result; CHECK(router.requested(result) == (buttons == 0x2008));
			if (buttons == 0x2008) CHECK(result == input_mode::Mode::Switch);
		}
	}
	input_mode::Router router; router.changeMode(input_mode::Mode::Switch); raw[8] = 0x88; raw[9] = 0x20; raw[10] = 0;
	for (unsigned i = 0; i < 4; ++i) router.observe(0,1,true,raw,1000+i*100000);
	input_mode::Mode result; CHECK(router.requested(result) && result == input_mode::Mode::Steam);
	router.reset(); raw[8] = 8; raw[9] = 0x20;
	for (unsigned i = 0; i < 4; ++i) router.observe(0,1,true,raw,1000+i*100000,false);
	CHECK(!router.requested(result));
	router.reset();
	for (unsigned i = 0; i < 4; ++i) router.observe(0,1,true,raw,0xffff0000u+i*100000);
	CHECK(router.requested(result) && result == input_mode::Mode::Switch);
	router.reset(); raw[8] = raw[9] = 0; router.observe(0,1,true,raw,1000);
	raw[8] = 8; raw[9] = 0x20;
	for (unsigned i = 0; i < 4; ++i) router.observe(0,1,true,raw,4000000+i*100000);
	CHECK(!router.requested(result));
	auto config = input_mode::defaults(); auto boot = input_mode::bootRequest(config, input_mode::Mode::Switch);
	uint8_t record[48]; uint32_t generation; input_mode::encodeRecord(config, 18, record, boot);
	CHECK(input_mode::decodeRecord(record, config, generation, &boot));
	CHECK(input_mode::consumeBoot(config, boot) == input_mode::Mode::Switch && input_mode::consumeBoot(config, boot) == input_mode::Mode::Steam);
	config.flags |= input_mode::PERSIST_MODE; boot = input_mode::bootRequest(config,input_mode::Mode::Switch);
	CHECK(input_mode::consumeBoot(config,boot) == input_mode::Mode::Switch && input_mode::consumeBoot(config,boot) == input_mode::Mode::Switch);
	sc::Receiver receiver; sc::Database db{}; CHECK(receiver.begin(db, {aes,random,persist,usb::input,usb::desktop,micros,usb::observeInput}));
	usb::configureBoot(input_mode::Mode::Switch, nullptr, nullptr); CHECK(usb::begin());
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(receiver,16);
	CHECK(TinyUSBDevice.vid == 0x057e && TinyUSBDevice.pid == 0x2009 && TinyUSBDevice.device_version == 0x0220);
	CHECK(TinyUSBDevice.descriptors.size() == 1 && TinyUSBDevice.descriptors[0].size() == 32);
	auto *hid = Adafruit_USBD_HID::instances[0]; hid->strict_in_flight = false; hid->sent.clear(); mock_micros += 4000; usb::pump(); CHECK(hid->sent.empty());
	uint8_t handshake[] = {0x80,2}; CHECK(mockHidOutput(0x80, handshake,2,2)); usb::pump();
	CHECK(hid->sent.size() == 1 && hid->sent[0].size() == 64 && hid->sent[0][0] == 0x81 && hid->sent[0][1] == 2);
	CHECK(!mockHidOutput(0x80, handshake,2,1)); CHECK(!mockHidOutput(0x80, handshake,2,2,XFER_RESULT_FAILED));
	CHECK(mockHidOutput(0x80,handshake,2,2,XFER_RESULT_SUCCESS,false));
	switch_pro::Report r; CHECK(!switch_pro::nextReport(neutral,mock_micros,r));
	tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true); CHECK(!mockFeatureAck()); usb::process(receiver,16);
	CHECK(!switch_pro::nextReport(neutral,mock_micros,r));
	usb::configureBoot(input_mode::Mode::Steam,nullptr,nullptr); CHECK(usb::begin());
	CHECK(TinyUSBDevice.descriptors.size() == 5 && !hid->out_endpoint && Adafruit_USBD_HID::instances.size() == 5);
	tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true); usb::process(receiver,16);
}
}
void testSwitch()
{
	testProtocol(); testMapping(); testRumble(); testChordsAndUsb();
	std::printf("PASS: Switch handshake, mapping, calibration, rumble codecs, boot chords and lifecycle (%u assertions; software only)\n", assertions);
}
