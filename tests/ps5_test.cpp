#include "ps5.h"
#include "input_mode.h"
#include "protocol.h"
#include "usb.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <device/dcd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { std::fprintf(stderr, "PS5 FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
void put16(uint8_t *p, int value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
void buttons(uint8_t *p, uint32_t value) { p[8] = uint8_t(value); p[9] = uint8_t(value >> 8); p[10] = uint8_t(value >> 16); }
bool aes(const uint8_t *, const uint8_t *, uint8_t *) { return false; }
bool random(uint8_t *p, size_t n) { memset(p, 7, n); return true; }
bool persist(const sc::Database &) { return true; }

void testMapping()
{
	ps5::enable(true, 0x12345678, 0xabcdef01);
	input_mode::Router router; router.changeMode(input_mode::Mode::PS5);
	uint8_t raw[64] = {1,0,1,60}, mapped[20], out[63];
	router.xboxReport(0, mapped); ps5::buildReport(mapped, 0, out);
	CHECK(out[0] == 128 && out[1] == 128 && out[2] == 128 && out[3] == 128);
	CHECK(out[7] == 8 && out[8] == 0 && out[9] == 0 && out[32] == 0x80 && out[36] == 0x80);
	const uint32_t physical[] = {1u<<7,1u<<5,1u<<6,1u<<4,1u<<3,1u<<2,1u<<12,1u<<14,1u<<13,1u<<15,1u<<16,1u<<22,1u<<18};
	const uint8_t offsets[] = {7,7,7,7,8,8,8,8,9,8,8,8,8};
	const uint8_t masks[] = {0x20,0x40,0x10,0x80,1,2,16,32,1,128,64,64,128};
	for (unsigned i = 0; i < sizeof(offsets); ++i) {
		buttons(raw, physical[i]); router.observe(0, 1, true, raw, 1000, false);
		router.xboxReport(1000, mapped); ps5::buildReport(mapped, 1000, out);
		CHECK(out[offsets[i]] == uint8_t(masks[i] | (offsets[i] == 7 ? 8 : 0)));
	}
	buttons(raw, 1u << 20); raw[11] = 123; raw[12] = 234;
	put16(raw + 16, -32768); put16(raw + 18, 32767);
	put16(raw + 20, 32767); put16(raw + 22, -32768);
	router.observe(0, 1, true, raw, 2000, false); router.xboxReport(2000, mapped); ps5::buildReport(mapped, 2000, out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 255 && out[3] == 255);
	CHECK(out[4] == 123 && out[5] == 234 && out[8] == 12);
	for (const auto &direction : {std::array<int,3>{0,20000,0}, {20000,20000,1}, {20000,0,2}, {20000,-20000,3},
		{0,-20000,4}, {-20000,-20000,5}, {-20000,0,6}, {-20000,20000,7}}) {
		buttons(raw, (1u << 17) | (1u << 19)); put16(raw + 16, direction[0]); put16(raw + 18, direction[1]);
		router.observe(0, 1, true, raw, 3000, false); router.xboxReport(3000, mapped); ps5::buildReport(mapped, 3000, out);
		CHECK((out[7] & 15) == direction[2]);
	}
	// PS5 selects its own diagonal width and inversion profile.
	auto config = input_mode::defaults(); input_mode::setDiagonalDegrees(config.ps5, 15); config.ps5.flags |= 3;
	router.configure(config); buttons(raw, (1u << 17) | (1u << 19) | (1u << 20));
	put16(raw + 16, 20000); put16(raw + 18, 10000); put16(raw + 20, 20000); put16(raw + 22, 20000);
	router.observe(0, 1, true, raw, 4000, false); router.xboxReport(4000, mapped); ps5::buildReport(mapped, 4000, out);
	CHECK((out[7] & 15) == 2 && out[2] < 128 && out[3] > 128);
	router.xboxReport(254001, mapped); ps5::buildReport(mapped, 254001, out);
	CHECK(out[0] == 128 && out[2] == 128 && out[4] == 0 && out[7] == 8 && out[8] == 0);
}
void testChordsAndRecords()
{
	uint8_t raw[64] = {1,0,1,60};
	for (auto start : {input_mode::Mode::Steam, input_mode::Mode::Xbox}) {
		for (uint32_t mask : {0x10000u,0x2000u,0x12000u,0x1a008u}) {
			input_mode::Router router; router.changeMode(start); buttons(raw, mask);
			for (unsigned i = 0; i < 4; ++i) router.observe(0, 1, true, raw, 1000 + i * 100000);
			input_mode::Mode target;
			CHECK(router.requested(target) == (mask == 0x12000));
			if (mask == 0x12000) {
				CHECK(target == input_mode::Mode::PS5); router.changeMode(target);
				CHECK(router.observe(0, 1, true, raw, 401000) && !router.requested(target));
				buttons(raw, 0x2000); CHECK(router.observe(0, 1, true, raw, 501000));
				buttons(raw, 0); CHECK(!router.observe(0, 1, true, raw, 601000));
			}
		}
	}
	for (uint32_t mask : {0x2080u,0x12080u,0x1a080u}) {
		input_mode::Router router; router.changeMode(input_mode::Mode::PS5); buttons(raw, mask);
		for (unsigned i = 0; i < 4; ++i) router.observe(0, 1, true, raw, 1000 + i * 100000);
		input_mode::Mode target; CHECK(router.requested(target) && target == input_mode::Mode::Steam);
	}
	input_mode::Router router; buttons(raw, 0); router.observe(0, 1, true, raw, 1000);
	buttons(raw, 0x12000);
	for (unsigned i = 0; i < 4; ++i) router.observe(0, 1, true, raw, 4000000 + i * 100000);
	input_mode::Mode target; CHECK(!router.requested(target));
	router.reset();
	for (unsigned i = 0; i < 4; ++i) router.observe(0, 1, true, raw, 1000 + i * 100000, false);
	CHECK(!router.requested(target));
	router.reset();
	for (unsigned i = 0; i < 4; ++i) router.observe(0, 1, true, raw, 0xffff0000u + i * 100000);
	CHECK(router.requested(target) && target == input_mode::Mode::PS5);
	auto config = input_mode::defaults(); uint8_t record[input_mode::RECORD_SIZE]; uint32_t generation;
	for (bool persistent : {false,true}) {
		if (persistent) config.flags |= input_mode::PERSIST_MODE;
		auto boot = input_mode::bootRequest(config, input_mode::Mode::PS5);
		input_mode::encodeRecord(config, 17, record, boot);
		CHECK(input_mode::decodeRecord(record, config, generation, &boot) && generation == 17);
		CHECK(input_mode::consumeBoot(config, boot) == input_mode::Mode::PS5);
		CHECK(input_mode::consumeBoot(config, boot) == (persistent ? input_mode::Mode::PS5 : input_mode::Mode::Steam));
	}
	CHECK(!input_mode::validMode(5));
}
void testUsb()
{
	sc::Receiver receiver; sc::Database db{};
	CHECK(receiver.begin(db, {aes,random,persist,usb::input,usb::desktop,micros,usb::observeInput}));
	usb::configureBoot(input_mode::Mode::PS5, nullptr, nullptr); usb::configureInput(input_mode::defaults(), nullptr);
	CHECK(usb::begin()); tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(receiver, 16);
	CHECK(TinyUSBDevice.vid == 0x054c && TinyUSBDevice.pid == 0x0ce6 && TinyUSBDevice.device_version == 0x0110);
	CHECK(TinyUSBDevice.descriptors.size() == 1 && TinyUSBDevice.descriptors[0].size() == 32);
	CHECK(TinyUSBDevice.descriptors[0][4] == 2 && TinyUSBDevice.descriptors[0][22] == 64);
	auto *hid = Adafruit_USBD_HID::instances[0]; CHECK(hid->protocol == 0 && hid->out_endpoint);
	hid->strict_in_flight = false; hid->sent.clear(); mock_micros += 4000; usb::pump();
	CHECK(hid->sent.size() == 1 && hid->sent[0].size() == 64 && hid->sent[0][0] == 1 && hid->sent[0][8] == 8);
	uint8_t buffer[65]; memset(buffer, 0xcd, sizeof(buffer));
	CHECK(ps5::getReport(5, HID_REPORT_TYPE_FEATURE, buffer + 1, 40) == 40);
	CHECK(buffer[0] == 0xcd && buffer[41] == 0xcd && get16(buffer + 7) == 8192);
	CHECK(ps5::getReport(5, HID_REPORT_TYPE_FEATURE, buffer, 3) == 3);
	CHECK(ps5::getReport(9, HID_REPORT_TYPE_FEATURE, buffer, 64) == 19 && buffer[0] == 2);
	uint8_t mac[6]; memcpy(mac, buffer, 6); ps5::reset();
	CHECK(ps5::getReport(9, HID_REPORT_TYPE_FEATURE, buffer, 64) == 19 && !memcmp(mac, buffer, 6));
	CHECK(ps5::getReport(0x20, HID_REPORT_TYPE_FEATURE, buffer, 64) == 63 && get16(buffer + 43) == 0x0224);
	CHECK(ps5::getReport(3, HID_REPORT_TYPE_FEATURE, buffer, 64) == 47 && buffer[1] == 0x28 && buffer[3] == 10);
	CHECK(ps5::getReport(0xf9, HID_REPORT_TYPE_FEATURE, buffer, 64) == 0);
	CHECK(ps5::getReport(1, HID_REPORT_TYPE_INPUT, buffer, 63) == 63 && buffer[0] == 128);
	uint8_t output[65] = {2,3,0,17,34}; rumble::Command command;
	ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 48); CHECK(!ps5::takeRumble(command));
	uint32_t epoch = ps5::setRumbleTarget(0, 17); CHECK(epoch != 0);
	ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 48); CHECK(ps5::takeRumble(command));
	CHECK(command.low == 34 && command.high == 17 && command.session == 17 && command.usb_generation == epoch);
	output[1] = 2; output[39] = 4; // payload starts with 02, not a duplicate report ID
	ps5::setReport(2, HID_REPORT_TYPE_OUTPUT, output + 1, 47); CHECK(ps5::takeRumble(command) && command.low == 34);
	for (unsigned length : {0u,4u,47u,65u}) { ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, uint16_t(length)); CHECK(!ps5::takeRumble(command)); }
	for (unsigned length : {48u,63u,64u}) {
		ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, uint16_t(length));
		CHECK(ps5::takeRumble(command) && command.low == 34 && command.high == 17);
		CHECK(mockPs5Output(output, length, length)); CHECK(ps5::takeRumble(command));
	}
	output[62] = 1; ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 63); CHECK(!ps5::takeRumble(command)); output[62] = 0;
	ps5::setReport(2, HID_REPORT_TYPE_FEATURE, output + 1, 47); CHECK(!ps5::takeRumble(command));
	memset(output + 1, 0, 48); output[2] = 4;
	ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 48); CHECK(!ps5::takeRumble(command)); // LED only
	output[2] = 0; ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 48);
	CHECK(ps5::takeRumble(command) && command.low == 0 && command.high == 0); // SDL all-zero stop
	output[1] = 3; output[3] = 17; output[4] = 34;
	CHECK(mockPs5Output(output, 48, 48, XFER_RESULT_SUCCESS, false)); CHECK(!ps5::takeRumble(command));
	CHECK(mockFeatureAck()); CHECK(ps5::takeRumble(command) && command.low == 34 && command.high == 17);
	CHECK(!mockPs5Output(output, 48, 47)); CHECK(!ps5::takeRumble(command));
	CHECK(!mockPs5Output(output, 64, 48)); CHECK(!ps5::takeRumble(command));
	CHECK(!mockPs5Output(output, 48, 48, XFER_RESULT_FAILED)); CHECK(!ps5::takeRumble(command));
	output[0] = 0xaf; CHECK(!mockPs5Output(output, 48, 48)); CHECK(!ps5::takeRumble(command)); output[0] = 2;
	CHECK(mockPs5Output(output, 48, 48, XFER_RESULT_SUCCESS, false));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); CHECK(!mockFeatureAck()); CHECK(!ps5::takeRumble(command));
	usb::process(receiver, 16); CHECK(ps5::setRumbleTarget(0, 17) != epoch);
	ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 48); ps5::setRumbleTarget(1, 18); CHECK(!ps5::takeRumble(command));
	TinyUSBDevice.suspended_state = true; CHECK(ps5::setRumbleTarget(1, 18) == 0);
	ps5::setReport(0, HID_REPORT_TYPE_OUTPUT, output, 48); CHECK(!ps5::takeRumble(command)); TinyUSBDevice.suspended_state = false;
	usb::configureBoot(input_mode::Mode::Steam, nullptr, nullptr); CHECK(usb::begin());
	CHECK(TinyUSBDevice.descriptors.size() == 5 && !hid->out_endpoint && hid->protocol == HID_ITF_PROTOCOL_KEYBOARD);
	CHECK(Adafruit_USBD_HID::instances.size() == 5);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(receiver, 16);
}
}
void testPs5()
{
	testMapping(); testChordsAndRecords(); testUsb();
	std::printf("PASS: PS5 mapping, startup chords, boot records, feature/output handling and lifecycle (%u assertions; software only)\n", assertions);
}
