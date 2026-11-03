#include "input_mode.h"
#include "protocol.h"
#include "usb.h"
#include "xinput.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <device/dcd.h>
#include <device/usbd_pvt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned assertions;
#define CHECK(x) do { ++assertions; if (!(x)) { std::fprintf(stderr, "INPUT MODE FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using input_mode::Mode;
void put16(uint8_t *p, int v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t *p) { return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16); }
void buttons(uint8_t *r, uint32_t b) { r[8] = uint8_t(b); r[9] = uint8_t(b >> 8); r[10] = uint8_t(b >> 16); }
void testMapping()
{
	input_mode::Router router;
	router.changeMode(Mode::Xbox);
	uint8_t r[64] = {1,0,1,60}, out[20];
	buttons(r, (1u << 15) | (1u << 16) | (1u << 18) | (1u << 20) | (1u << 7));
	put16(r + 16, 32767); put16(r + 18, -32768);
	put16(r + 20, -32768); put16(r + 22, 32767);
	r[11] = 123; r[12] = 234;
	CHECK(!router.observe(0, 1, true, r, 1000)); router.xboxReport(1000, out);
	CHECK(out[0] == 0 && out[1] == 20 && get16(out + 2) == 0x10c0);
	CHECK(get16(out + 6) == 32767 && int16_t(get16(out + 8)) == -32767);
	CHECK(int16_t(get16(out + 10)) == -32767 && get16(out + 12) == 32767);
	CHECK(out[4] == 123 && out[5] == 234);
	for (unsigned i = 14; i < 20; ++i) CHECK(out[i] == 0);
	// Pad and stick alternate: pad sample must not become left-stick motion.
	buttons(r, (1u << 17) | (1u << 19) | (1u << 23)); put16(r + 16, -20000); put16(r + 18, 20000);
	router.observe(0, 1, true, r, 2000); router.xboxReport(2000, out);
	CHECK((get16(out + 2) & 15) == 5 && get16(out + 6) == 32767);
	CHECK(get16(out + 10) == 0 && get16(out + 12) == 0); // release right touch -> center
	buttons(r, (1u << 17) | (1u << 23)); put16(r + 16, 0); put16(r + 18, 0);
	router.observe(0, 1, true, r, 3000); router.xboxReport(3000, out);
	CHECK((get16(out + 2) & 15) == 5 && get16(out + 6) == 0);
	buttons(r, 0); router.observe(0, 1, true, r, 4000); router.xboxReport(4000, out);
	CHECK((get16(out + 2) & 15) == 0);
	// Pad-only frames explicitly center the physical stick.
	buttons(r, (1u << 17) | (1u << 19)); put16(r + 16, 20000); put16(r + 18, -20000);
	router.observe(0, 1, true, r, 5000); router.xboxReport(5000, out);
	CHECK((get16(out + 2) & 15) == 10 && get16(out + 6) == 0);
	// User scenario: LEFT pad physically pressed to the left, physical stick
	// held RIGHT. Interleaving must never turn the stick left or release D-left.
	for (unsigned i = 0; i < 40; ++i) {
		buttons(r, (1u << 17) | (1u << 23) | ((i & 1) ? 0 : (1u << 19)));
		put16(r + 16, (i & 1) ? 20000 : -20000); put16(r + 18, 0);
		router.observe(0, 1, true, r, 5100 + i * 10); router.xboxReport(5100 + i * 10, out);
		CHECK((get16(out + 2) & 15) == 4);
		if (i) CHECK(int16_t(get16(out + 6)) > 0);
	}
	// User mappings, deadzones, and invert never overflow INT16_MIN.
	auto config = input_mode::defaults(); config.bindings[9] = input_mode::B; config.flags |= 3;
	router.configure(config); buttons(r, (1u << 15) | (1u << 20));
	put16(r + 20, -32768); put16(r + 22, 1000);
	router.observe(0, 1, true, r, 6000); router.xboxReport(6000, out);
	CHECK(get16(out + 2) == 0x2000 && get16(out + 10) == 32767 && get16(out + 12) == 0);
	config.bindings[12] = input_mode::A; router.configure(config);
	buttons(r, (1u << 17) | (1u << 22));
	router.observe(0, 1, true, r, 6100); router.xboxReport(6100, out);
	CHECK(get16(out + 2) == 0x40); // compatibility mirror is a stick click, not pad A
	// A second live slot cannot steal the active gamepad; silence is neutral.
	router.observe(1, 1, true, r, 7000); CHECK(router.owner() == 0);
	router.xboxReport(300001, out); CHECK(get16(out + 2) == 0 && out[4] == 0);
	router.observe(0, 2, false, r, 300002); router.observe(1, 1, true, r, 300003);
	CHECK(router.owner() == 1);
}
void testChord()
{
	uint8_t r[64] = {1,0,1,60}; Mode mode;
	for (uint32_t mask : {0x8000u, 0x2000u, 0xa000u}) {
		input_mode::Router router; buttons(r, mask);
		for (uint32_t t = 1000; t <= 301000; t += 100000) router.observe(0, 2, true, r, t);
		CHECK(router.requested(mode) == (mask == 0xa000));
		if (mask == 0xa000) {
			CHECK(mode == Mode::Xbox && router.suppressDesktop());
			router.changeMode(mode);
			for (uint32_t t = 401000; t <= 901000; t += 100000) CHECK(router.observe(0, 2, true, r, t));
			CHECK(!router.requested(mode));
			buttons(r, 0x8000); CHECK(router.observe(0, 2, true, r, 950000));
			uint8_t held[20]; router.xboxReport(950000, held); CHECK(get16(held + 2) == 0);
			buttons(r, 0xa000);
			router.unplug(); CHECK(router.mode() == Mode::Steam);
			for (uint32_t t = 1001000; t <= 1401000; t += 100000) router.observe(0, 2, true, r, t);
			CHECK(!router.requested(mode));
		}
	}
	input_mode::Router router; buttons(r, 0); router.observe(0, 1, true, r, 1000);
	buttons(r, 0xa000);
	for (uint32_t t = 4000000; t < 4500000; t += 100000) router.observe(0, 1, true, r, t);
	CHECK(!router.requested(mode)); // not a normal gameplay chord
	router.reset(); buttons(r, 0xa000);
	for (uint32_t t = 0xffff0000u, i = 0; i < 4; ++i, t += 100000) router.observe(0, 1, true, r, t);
	CHECK(router.requested(mode)); // natural microsecond wrap
	router.reset();
	for (uint32_t t = 1000; t < 500000; t += 100000) router.observe(0, 1, true, r, t, false);
	CHECK(!router.requested(mode)); // pending pairing is not startup switching
}
void testDpadDirections()
{
	input_mode::Router router; router.changeMode(Mode::Xbox);
	uint8_t r[64] = {1,0,1,60}, out[20];
	const int points[8][3] = {{0,20000,1},{20000,20000,9},{20000,0,8},{20000,-20000,10},
		{0,-20000,2},{-20000,-20000,6},{-20000,0,4},{-20000,20000,5}};
	for (unsigned i = 0; i < 8; ++i) {
		buttons(r, (1u << 17) | (1u << 19)); put16(r + 16, points[i][0]); put16(r + 18, points[i][1]);
		router.observe(0, 1, true, r, 1000 + i); router.xboxReport(1000 + i, out);
		CHECK(get16(out + 2) == points[i][2]);
	}
	put16(r + 16, 10000); put16(r + 18, 5000);
	router.observe(0, 1, true, r, 2000); router.xboxReport(2000, out); CHECK(get16(out + 2) == 9);
	put16(r + 16, 2000); put16(r + 18, 2000);
	router.observe(0, 1, true, r, 2001); router.xboxReport(2001, out); CHECK(get16(out + 2) == 0);
	buttons(r, 1u << 19);
	put16(r + 16, -20000); put16(r + 18, 0);
	router.observe(0, 1, true, r, 2002); router.xboxReport(2002, out); CHECK(get16(out + 2) == 0);
	buttons(r, (1u << 19) | (1u << 17));
	router.observe(0, 1, true, r, 2003); router.xboxReport(2003, out); CHECK(get16(out + 2) == 4);
	// Release the click while staying on the left side: no held D-pad remains.
	buttons(r, 1u << 19); router.observe(0, 1, true, r, 2004); router.xboxReport(2004, out); CHECK(get16(out + 2) == 0);
	auto config = input_mode::defaults(); config.flags = 0; router.configure(config);
	router.observe(0, 1, true, r, 2005); router.xboxReport(2005, out); CHECK(get16(out + 2) == 4);
}
void testRecord()
{
	auto config = input_mode::defaults();
	uint8_t record[input_mode::RECORD_SIZE]; input_mode::encodeRecord(config, 0xffffffffu, record);
	input_mode::Config decoded{}; uint32_t generation;
	CHECK(input_mode::decodeRecord(record, decoded, generation) && generation == 0xffffffffu);
	for (unsigned i = 0; i < sizeof(record); ++i) {
		record[i] ^= 1; CHECK(!input_mode::decodeRecord(record, decoded, generation)); record[i] ^= 1;
	}
	for (unsigned count = 0; count < sizeof(record); ++count) {
		uint8_t torn[input_mode::RECORD_SIZE]; memset(torn, 255, sizeof(torn)); memcpy(torn, record, count);
		CHECK(!input_mode::decodeRecord(torn, decoded, generation));
	}
	config.bindings[0] = input_mode::TargetCount; CHECK(!input_mode::valid(config));
}
bool aes(const uint8_t *, const uint8_t *, uint8_t *) { return false; }
bool random(uint8_t *p, size_t n) { memset(p, 0x42, n); return true; }
bool persistBond(const sc::Database &) { return true; }
unsigned saves;
bool fail_save;
bool persistConfig(const input_mode::Config &) { ++saves; return !fail_save; }
extern "C" const usbd_class_driver_t *__wrap_usbd_app_driver_get_cb(uint8_t *);
extern "C" void tud_event_hook_cb(uint8_t, uint32_t, bool);
void transact(sc::Receiver &r, const uint8_t *data, unsigned size, uint8_t *reply, unsigned get_size = 64, bool complete = true)
{
	unsigned base = usb::inputMode() == Mode::Xbox ? 2 : 1;
	CHECK(mockFeatureSet(base, data, size, size));
	usb::process(r, 16); usb::publish(r);
	CHECK(Adafruit_USBD_HID::instances[1]->get(0, HID_REPORT_TYPE_FEATURE, reply, uint16_t(get_size), complete) == get_size);
	usb::process(r, 16);
}
void settle(sc::Receiver &r)
{
	CHECK(usb::serviceMode(r)); usb::process(r, 16);
	mock_micros += 80000; CHECK(usb::serviceMode(r));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(r, 16); CHECK(usb::serviceMode(r)); usb::publish(r);
}
void testUsbMode()
{
	sc::Receiver r; sc::Database db{}; db.address[0] = 0x28;
	CHECK(r.begin(db, {aes, random, persistBond, usb::input, usb::desktop, micros}));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(r, 16); CHECK(usb::serviceMode(r)); usb::publish(r);
	usb::configureInput(input_mode::defaults(), persistConfig);
	uint8_t reply[64], enter[] = {0xfd,5,'M','O','D','E',1};
	transact(r, enter, sizeof(enter), reply, 8); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam); // short GET cannot switch
	transact(r, enter, sizeof(enter), reply, 64, false); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam); // aborted full GET cannot switch
	const uint8_t other_reply[] = {0xf0,0};
	CHECK(r.feature(0, other_reply, sizeof(other_reply)));
	tusb_control_request_t stale_get{0xa1,1,0x0300,1,64};
	dcd_edpt0_status_complete(0, &stale_get); usb::process(r, 16); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam); // an invalidated publication cannot arm the switch
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r, 16);
	transact(r, enter, sizeof(enter), reply);
	CHECK(reply[0] == 0xfd && reply[4] == 0 && reply[5] == 0 && reply[6] == 1);
	settle(r); CHECK(usb::inputMode() == Mode::Xbox);
	CHECK(TinyUSBDevice.vid == 0x28de && TinyUSBDevice.pid == 0x1143);
	CHECK(xinput::osString() && xinput::osString()[0] == 0x0312);
	tusb_control_request_t os_request{0xc0,0x20,0,4,40};
	CHECK(xinput::vendorControl(0, CONTROL_STAGE_SETUP, &os_request));
	os_request.wIndex = 5; CHECK(!xinput::vendorControl(0, CONTROL_STAGE_SETUP, &os_request));
	CHECK(TinyUSBDevice.descriptors.size() == 6 && Adafruit_USBD_HID::instances.size() == 5);
	CHECK(Adafruit_USBD_HID::instances[1]->interface_number == 2);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Xbox); // host reset is not physical unplug
	uint8_t count; const auto *driver = __wrap_usbd_app_driver_get_cb(&count);
	CHECK(count == 1); mock_xinput::reset(); driver->reset(0);
	const auto &descriptor = TinyUSBDevice.descriptors[0];
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data()), 39) == 0);
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data()), 40) == 40);
	uint8_t report[20] = {0,20,0,0x10}; CHECK(xinput::send(report)); CHECK(!xinput::send(report));
	CHECK(mock_xinput::sent.back() == std::vector<uint8_t>(report, report + 20));
	mock_xinput::busy[0x81] = false; mock_xinput::fail_transfer = true;
	CHECK(!xinput::send(report) && !mock_xinput::claimed[0x81]); mock_xinput::fail_transfer = false;
	uint8_t read[] = {0xfe,0}; transact(r, read, 2, reply);
	CHECK(reply[4] == 0 && reply[5] == input_mode::CONFIG_SIZE);
	uint8_t write[31] = {0xfe,29,1}; memcpy(write + 3, reply + 8, 4); memcpy(write + 7, reply + 12, 24);
	write[7 + 2 + 9] = input_mode::A;
	uint32_t previous = get32(reply + 8);
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 0 && saves == 1 && get32(reply + 8) != previous);
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 2 && saves == 1);
	memcpy(write + 3, reply + 8, 4); write[7 + 2 + 9] = input_mode::B; fail_save = true;
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 6 && reply[12 + 2 + 9] == input_mode::A);
	fail_save = false; write[8] = 0xff;
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 1);
	tud_event_hook_cb(0, DCD_EVENT_UNPLUGGED, true); settle(r);
	CHECK(usb::inputMode() == Mode::Steam && TinyUSBDevice.vid == 0x28de && TinyUSBDevice.pid == 0x1142);
	CHECK(Adafruit_USBD_HID::instances.size() == 5 && Adafruit_USBD_HID::instances[1]->interface_number == 1);
	CHECK(!xinput::send(report));
	CHECK(!xinput::osString());
	transact(r, read, 2, reply); CHECK(reply[12 + 2 + 9] == input_mode::A); // configuration survives mode reset
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
}
}
void testInputMode()
{
	testMapping(); testChord(); testDpadDirections(); testRecord(); testUsbMode();
	std::printf("PASS: input mapping, startup chord, config journal, XInput driver and USB mode lifecycle (%u assertions; software only)\n", assertions);
}
