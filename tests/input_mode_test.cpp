#include "input_mode.h"
#include "protocol.h"
#include "usb.h"
#include "xinput.h"
#include "usb_tx_guard.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <device/dcd.h>
#include <device/usbd_pvt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

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
	for (uint32_t mask : {0x80u,0x2000u,0x2080u,0xa080u}) {
		input_mode::Router back; back.changeMode(Mode::Xbox); buttons(r, mask);
		for (uint32_t t = 1000; t <= 301000; t += 100000) back.observe(0, 5, true, r, t);
		CHECK(back.requested(mode) == (mask == 0x2080 || mask == 0xa080));
		if (mask == 0x2080 || mask == 0xa080) {
			CHECK(mode == Mode::Steam); back.changeMode(mode);
			CHECK(back.observe(0, 5, true, r, 401000) && !back.requested(mode));
			buttons(r, 0x80); CHECK(back.observe(0, 5, true, r, 501000));
			buttons(r, 0); CHECK(!back.observe(0, 5, true, r, 601000));
		}
	}
	// Switching chord identity midway must restart the continuous hold timer.
	input_mode::Router back; back.changeMode(Mode::Xbox); buttons(r, 0xa000);
	back.observe(0, 6, true, r, 1000); back.observe(0, 6, true, r, 101000);
	buttons(r, 0x2080); back.observe(0, 6, true, r, 201000); back.observe(0, 6, true, r, 301000);
	CHECK(!back.requested(mode)); back.observe(0, 6, true, r, 401000); back.observe(0, 6, true, r, 501000);
	CHECK(back.requested(mode) && mode == Mode::Steam);
	back.reset(); back.changeMode(Mode::Xbox); buttons(r, 0); back.observe(0, 7, true, r, 1000);
	buttons(r, 0x2080);
	for (uint32_t t = 4000000; t < 4500000; t += 100000) back.observe(0, 7, true, r, t);
	CHECK(!back.requested(mode));
	back.reset(); back.changeMode(Mode::Xbox);
	for (uint32_t t = 1000; t < 500000; t += 100000) back.observe(0, 8, true, r, t, false);
	CHECK(!back.requested(mode));
	back.reset(); back.changeMode(Mode::Xbox);
	for (unsigned i = 0; i < 4; ++i) back.observe(0, 9, true, r, 0xffff0000u + i * 100000);
	CHECK(back.requested(mode) && mode == Mode::Steam);
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
	router.observe(0, 1, true, r, 2000); router.xboxReport(2000, out); CHECK(get16(out + 2) == 8);
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
	// Sweep both sides of the angular boundaries in every quadrant. The
	// expectation uses angles, independently of the production tangent table.
	for (unsigned degrees = 5; degrees <= 75; degrees += 5) {
		CHECK(input_mode::setDiagonalDegrees(config, uint8_t(degrees)));
		CHECK(input_mode::diagonalDegrees(config) == degrees);
		router.configure(config);
		for (int sx : {-1, 1}) for (int sy : {-1, 1}) for (int inside : {-1, 1}) {
			double angle = ((90.0 - degrees) / 2 + inside) * 3.141592653589793 / 180.0;
			int x = int(20000 * std::cos(angle)), y = int(20000 * std::sin(angle));
			uint16_t horizontal = sx < 0 ? 4 : 8, vertical = sy < 0 ? 2 : 1;
			put16(r + 16, sx * x); put16(r + 18, sy * y);
			router.observe(0, 1, true, r, 3000); router.xboxReport(3000, out);
			CHECK(get16(out + 2) == (inside < 0 ? horizontal : (horizontal | vertical)));
			put16(r + 16, sx * y); put16(r + 18, sy * x);
			router.observe(0, 1, true, r, 3001); router.xboxReport(3001, out);
			CHECK(get16(out + 2) == (inside < 0 ? vertical : (horizontal | vertical)));
		}
	}
	CHECK(input_mode::setDiagonalDegrees(config, 0)); router.configure(config);
	put16(r + 16, 20000); put16(r + 18, 20000);
	router.observe(0, 1, true, r, 4000); router.xboxReport(4000, out); CHECK(get16(out + 2) == 8);
	CHECK(!input_mode::setDiagonalDegrees(config, 31) && !input_mode::setDiagonalDegrees(config, 80));
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
	config = input_mode::defaults();
	auto boot = input_mode::bootRequest(config, Mode::Xbox);
	CHECK(boot.once == 1 && input_mode::consumeBoot(config, boot) == Mode::Xbox);
	CHECK(boot.once == 255 && input_mode::consumeBoot(config, boot) == Mode::Steam);
	config.flags |= input_mode::PERSIST_MODE;
	boot = input_mode::bootRequest(config, Mode::Xbox);
	CHECK(boot.once == 255 && input_mode::consumeBoot(config, boot) == Mode::Xbox);
	boot = input_mode::bootRequest(config, Mode::Steam);
	CHECK(input_mode::consumeBoot(config, boot) == Mode::Steam);
	config = input_mode::defaults(); boot = input_mode::bootRequest(config, Mode::Xbox);
	input_mode::encodeRecord(config, 7, record, boot);
	CHECK(input_mode::decodeRecord(record, decoded, generation, &boot) && boot.once == 1);
	// A power loss before consumption replays the durable one-shot once, just
	// as OpenPuck's bootMode does. A committed consumed record then boots Steam.
	CHECK(input_mode::consumeBoot(config, boot) == Mode::Xbox);
	input_mode::encodeRecord(config, 8, record, boot);
	CHECK(input_mode::decodeRecord(record, decoded, generation, &boot));
	CHECK(input_mode::consumeBoot(config, boot) == Mode::Steam);
	// Upgrade the previous 48-byte record without changing its mapping.
	record[4] = 1; record[13] &= 7; memset(record + 36, 0, 4);
	uint32_t crc = sc::crc32(record, 40); memcpy(record + 40, &crc, 4);
	CHECK(input_mode::decodeRecord(record, decoded, generation, &boot) && boot.once == 255);
	CHECK(decoded.bindings[8] == input_mode::Guide && !(decoded.flags & input_mode::PERSIST_MODE));
	CHECK(input_mode::diagonalDegrees(decoded) == 45 && input_mode::diagonalDegrees(input_mode::defaults()) == 30);
}
void testGuide()
{
	input_mode::Router router; router.changeMode(Mode::Xbox);
	uint8_t r[64] = {1,0,1,60}, out[20];
	buttons(r, 0x2000); CHECK(!router.observe(0, 1, true, r, 1000));
	router.xboxReport(1000, out); CHECK(get16(out + 2) == 0x0400);
	buttons(r, 0); router.observe(0, 1, true, r, 2000); router.xboxReport(2000, out);
	CHECK(get16(out + 2) == 0);
	// A held startup chord remains suppressed after boot; a fresh Guide press
	// after both keys release must still produce only Xbox bit 10.
	buttons(r, 0xa000);
	for (unsigned i = 0; i < 4; ++i) router.observe(0, 2, true, r, 10000 + i * 100000);
	router.xboxReport(310000, out); CHECK(get16(out + 2) == 0);
	buttons(r, 0); router.observe(0, 2, true, r, 320000);
	buttons(r, 0x2000); CHECK(!router.observe(0, 2, true, r, 330000));
	router.xboxReport(330000, out); CHECK(get16(out + 2) == 0x0400);
}
bool aes(const uint8_t *, const uint8_t *, uint8_t *) { return false; }
bool random(uint8_t *p, size_t n) { memset(p, 0x42, n); return true; }
bool persistBond(const sc::Database &) { return true; }
unsigned saves;
bool fail_save;
input_mode::Config saved_config = input_mode::defaults();
input_mode::BootState saved_boot = input_mode::defaultBootState();
unsigned boot_saves, restarts;
bool fail_boot_save;
bool persistConfig(const input_mode::Config &config) { ++saves; if (fail_save) return false; saved_config = config; return true; }
bool persistBoot(Mode mode) { ++boot_saves; if (fail_boot_save) return false;
	saved_boot = input_mode::bootRequest(saved_config, mode); return true; }
void restartMock() { ++restarts; }
extern "C" const usbd_class_driver_t *__wrap_usbd_app_driver_get_cb(uint8_t *);
extern "C" void tud_event_hook_cb(uint8_t, uint32_t, bool);
void transact(sc::Receiver &r, const uint8_t *data, unsigned size, uint8_t *reply, unsigned get_size = 64, bool complete = true)
{
	CHECK(usb::inputMode() == Mode::Steam);
	unsigned base = 1;
	CHECK(mockFeatureSet(base, data, size, size));
	usb::process(r, 16); usb::publish(r);
	CHECK(Adafruit_USBD_HID::instances[1]->get(0, HID_REPORT_TYPE_FEATURE, reply, uint16_t(get_size), complete) == get_size);
	usb::process(r, 16);
}
void boot(sc::Receiver &r)
{
	Mode mode = input_mode::consumeBoot(saved_config, saved_boot);
	usb::configureBoot(mode, persistBoot, restartMock);
	usb::configureInput(saved_config, persistConfig);
	CHECK(usb::begin());
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r, 16);
	CHECK(usb::serviceMode(r)); usb::publish(r);
}
void settle(sc::Receiver &r)
{
	CHECK(usb::serviceMode(r)); usb::process(r, 16);
	unsigned resets = restarts;
	mock_micros += 59999; CHECK(usb::serviceMode(r)); CHECK(restarts == resets);
	++mock_micros; CHECK(!usb::serviceMode(r)); CHECK(restarts == resets + 1);
	CHECK(!usb::serviceMode(r)); CHECK(restarts == resets + 1);
	boot(r);
}
void testUsbMode()
{
	TinyUSBDevice.unplug_on_detach = true;
	mock_power.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;
	sc::Receiver r; sc::Database db{}; db.address[0] = 0x28;
	CHECK(r.begin(db, {aes, random, persistBond, usb::input, usb::desktop, micros}));
	boot(r);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(r, 16); CHECK(usb::serviceMode(r)); usb::publish(r);
	usb::configureInput(input_mode::defaults(), persistConfig);
	uint8_t reply[64], enter[] = {0xfd,5,'M','O','D','E',1};
	transact(r, enter, sizeof(enter), reply, 8); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam && boot_saves == 0); // short GET cannot persist
	transact(r, enter, sizeof(enter), reply, 64, false); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam); // aborted full GET cannot switch
	const uint8_t other_reply[] = {0xf0,0};
	CHECK(r.feature(0, other_reply, sizeof(other_reply)));
	tusb_control_request_t stale_get{0xa1,1,0x0300,1,64};
	dcd_edpt0_status_complete(0, &stale_get); usb::process(r, 16); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam); // an invalidated publication cannot arm the switch
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r, 16);
	unsigned detached_before_failure = TinyUSBDevice.detach_count;
	fail_boot_save = true; transact(r, enter, sizeof(enter), reply); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Steam && TinyUSBDevice.detach_count == detached_before_failure && restarts == 0);
	const uint8_t mode_status[] = {0xfd,0}; transact(r, mode_status, 2, reply);
	CHECK(reply[8] & 16); fail_boot_save = false;
	transact(r, enter, sizeof(enter), reply);
	CHECK(reply[0] == 0xfd && reply[4] == 0 && reply[5] == 0 && reply[6] == 1);
	settle(r); CHECK(usb::inputMode() == Mode::Xbox);
	CHECK(TinyUSBDevice.vid == 0x045e && TinyUSBDevice.pid == 0x028e);
	CHECK(TinyUSBDevice.device_version == 0x0114);
	CHECK(xinput::osString() && xinput::osString()[0] == 0x0312);
	tusb_control_request_t os_request{0xc0,0x20,0,4,40};
	CHECK(xinput::vendorControl(0, CONTROL_STAGE_SETUP, &os_request));
	CHECK(mock_control_input.size() == 40 && get32(mock_control_input.data()) == 40);
	CHECK(mock_control_input[16] == 0 && !memcmp(mock_control_input.data() + 18, "XUSB10\0\0", 8));
	os_request.wLength = 16;
	CHECK(xinput::vendorControl(0, CONTROL_STAGE_SETUP, &os_request) && mock_control_input.size() == 16);
	os_request.wIndex = 5; CHECK(!xinput::vendorControl(0, CONTROL_STAGE_SETUP, &os_request));
	CHECK(TinyUSBDevice.descriptors.size() == 1 && TinyUSBDevice.interface_count == 4);
	CHECK(Adafruit_USBD_HID::instances.size() == 5); // no new HID registration in Xbox
	unsigned detaches = TinyUSBDevice.detach_count;
	for (unsigned i = 0; i < 20; ++i) {
		mock_micros += 10000; CHECK(usb::serviceMode(r)); usb::process(r, 16);
		CHECK(usb::inputMode() == Mode::Xbox);
	}
	CHECK(TinyUSBDevice.detach_count == detaches); // begin's second detach cannot undo Xbox
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r); CHECK(usb::serviceMode(r));
	CHECK(usb::inputMode() == Mode::Xbox); // host reset is not physical unplug
	uint8_t count; const auto *driver = __wrap_usbd_app_driver_get_cb(&count);
	CHECK(count == 1); mock_xinput::reset(); driver->reset(0);
	const auto &descriptor = TinyUSBDevice.descriptors[0];
	CHECK(descriptor.size() == 144 && xinput::securityString()[0] == 0x03b2);
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data() + 40), 64) == 64);
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data() + 104), 25) == 25);
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data() + 129), 15) == 15);
	CHECK(!mock_xinput::opened[0x83] && !mock_xinput::opened[0x86]);
	// MS-XUSBI table 46: five IN report lengths precede the OUT wReports.
	// Parse field offsets rather than comparing against the producer's bytes.
	CHECK(descriptor[9] == 17 && descriptor[10] == 0x21);
	CHECK(get16(descriptor.data() + 11) == 0x0100 && descriptor[13] == 1);
	CHECK(get16(descriptor.data() + 14) == 0x8125 && descriptor[16] == 20);
	CHECK(descriptor[17] == 0 && descriptor[18] == 0 && descriptor[19] == 0 && descriptor[20] == 0);
	CHECK(get16(descriptor.data() + 21) == 0x0213 && descriptor[23] == 8);
	CHECK(descriptor[24] == 0 && descriptor[25] == 0);
	CHECK(descriptor[26] == 7 && descriptor[28] == 0x81 && get16(descriptor.data() + 30) == 32);
	CHECK(descriptor[33] == 7 && descriptor[35] == 2 && get16(descriptor.data() + 37) == 32);
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data()), 39) == 0);
	mock_xinput::fail_transfer = true;
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data()), 40) == 40);
	CHECK(mock_xinput::opened[0x81] && mock_xinput::opened[2]);
	xinput::service(); CHECK(!mock_xinput::claimed[2]);
	mock_xinput::fail_transfer = false;
	xinput::service(); CHECK(mock_xinput::busy[2] && mock_xinput::out_capacity == 32);
	CHECK(mock_xinput::last_out_priority == TASK_PRIO_HIGH && mock_task_priority == TASK_PRIO_LOW);
	unsigned attempts = mock_xinput::out_attempts;
	xinput::service(); CHECK(mock_xinput::out_attempts == attempts);
	tusb_control_request_t get_input{0xa1,1,0x0100,0,20};
	CHECK(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &get_input));
	CHECK(mock_control_input.size() == 20 && mock_control_input[1] == 20);
	for (unsigned i = 2; i < 20; ++i) CHECK(mock_control_input[i] == 0);
	uint8_t before[64]{}; xinput::diagnostics(before);
	uint8_t report[20] = {0,20,0,0x10}; CHECK(xinput::send(report)); CHECK(!xinput::send(report));
	CHECK(mock_xinput::last_in_priority == TASK_PRIO_HIGH && mock_task_priority == TASK_PRIO_LOW);
	{ usb::TxGuard outer; { usb::TxGuard inner; CHECK(mock_task_priority == TASK_PRIO_HIGH); }
		CHECK(mock_task_priority == TASK_PRIO_HIGH); }
	CHECK(mock_task_priority == TASK_PRIO_LOW);
	CHECK(mock_xinput::sent.back() == std::vector<uint8_t>(report, report + 20));
	CHECK(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &get_input));
	CHECK(mock_control_input == std::vector<uint8_t>(report, report + 20));
	// Updating live input while EP0 is outstanding cannot change its snapshot.
	report[3] = 0x20; CHECK(!xinput::send(report)); CHECK(mock_control_pointer[3] == 0x10);
	CHECK(driver->xfer_cb(0, 0x81, XFER_RESULT_SUCCESS, 20));
	mock_xinput::busy[0x81] = false; mock_xinput::fail_transfer = true;
	CHECK(!xinput::send(report) && !mock_xinput::claimed[0x81]); mock_xinput::fail_transfer = false;
	get_input.wLength = 4;
	CHECK(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &get_input));
	CHECK(mock_control_input == std::vector<uint8_t>(report, report + 4));
	get_input.wLength = 64;
	CHECK(driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &get_input) && mock_control_input.size() == 20);
	CHECK(driver->control_xfer_cb(0, CONTROL_STAGE_ACK, &get_input));
	const tusb_control_request_t rejected[] = {
		{0x21,1,0x0100,0,20}, {0xa1,9,0x0100,0,20}, {0xa1,1,0x0300,0,20},
		{0xa1,1,0x0101,0,20}, {0xa1,1,0x0100,1,20}, {0xa1,1,0x0100,0x100,20}, {0xa1,1,0x0100,0,0},
	};
	for (const auto &request : rejected) CHECK(!driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
	CHECK(!driver->control_xfer_cb(1, CONTROL_STAGE_SETUP, &get_input));
	CHECK(driver->xfer_cb(0, 2, XFER_RESULT_SUCCESS, 8));
	CHECK(driver->xfer_cb(0, 2, XFER_RESULT_FAILED, 0));
	CHECK(!driver->xfer_cb(0, 3, XFER_RESULT_SUCCESS, 8));
	xinput::diagnostics(reply);
	CHECK(reply[9] == 1 && reply[10] == 7 && reply[11] == 0);
	CHECK(get32(reply + 28) - get32(before + 28) == 1);
	CHECK(get32(reply + 32) - get32(before + 32) == 1);
	CHECK(get32(reply + 36) - get32(before + 36) == 1);
	CHECK(get32(reply + 40) - get32(before + 40) == 1);
	CHECK(get32(reply + 44) - get32(before + 44) == 1);
	CHECK(get32(reply + 60) - get32(before + 60) == 7);
	CHECK(!memcmp(reply + 52, &rejected[6], 8));
	mock_xinput::busy[2] = false; mock_xinput::fail_transfer = true;
	CHECK(driver->xfer_cb(0, 2, XFER_RESULT_FAILED, 0));
	xinput::service(); CHECK(!mock_xinput::claimed[2]);
	mock_xinput::fail_transfer = false; TinyUSBDevice.suspended_state = true;
	attempts = mock_xinput::out_attempts; xinput::service(); CHECK(mock_xinput::out_attempts == attempts);
	TinyUSBDevice.suspended_state = false;
	xinput::service(); CHECK(mock_xinput::busy[2]);
	driver->reset(0); CHECK(!xinput::send(report));
	CHECK(!driver->control_xfer_cb(0, CONTROL_STAGE_SETUP, &get_input));
	xinput::diagnostics(reply);
	CHECK(!(reply[10] & 2) && get32(reply + 28) == get32(before + 28) + 1);
	mock_xinput::reset();
	CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data()), 40) == 40);
	auto output = [&](const uint8_t *data, unsigned size, xfer_result_t result = XFER_RESULT_SUCCESS) {
		CHECK(mock_xinput::out_buffer && size <= mock_xinput::out_capacity);
		memcpy(mock_xinput::out_buffer, data, size); mock_xinput::busy[2] = false;
		CHECK(driver->xfer_cb(0, 2, result, size));
	};
	uint8_t motors[8] = {0,8,0,255,128,0,0,0}; rumble::Command command;
	output(motors, 8); CHECK(!xinput::takeRumble(command)); // no owner at receipt
	uint32_t rumble_epoch = xinput::setRumbleTarget(0, 17); CHECK(rumble_epoch != 0);
	CHECK(!xinput::takeRumble(command));
	output(motors, 8); CHECK(xinput::takeRumble(command));
	CHECK(command.slot == 0 && command.session == 17 && command.usb_generation == rumble_epoch &&
	      command.low == 255 && command.high == 128 && command.at_us == mock_micros);
	CHECK(!xinput::takeRumble(command));
	for (unsigned i : {0u,1u,2u,5u,6u,7u}) {
		motors[i] ^= 0x80; output(motors, 8); CHECK(!xinput::takeRumble(command)); motors[i] ^= 0x80;
	}
	output(motors, 7); CHECK(!xinput::takeRumble(command));
	output(motors, 8, XFER_RESULT_FAILED); CHECK(!xinput::takeRumble(command));
	output(motors, 8); motors[3] = motors[4] = 0; output(motors, 8);
	CHECK(xinput::takeRumble(command) && command.low == 0 && command.high == 0); // latest stop wins
	output(motors, 8); xinput::setRumbleTarget(1, 18); CHECK(!xinput::takeRumble(command));
	output(motors, 8); tud_event_hook_cb(0, DCD_EVENT_SUSPEND, true);
	CHECK(!xinput::takeRumble(command));
	CHECK(xinput::setRumbleTarget(1, 18) != rumble_epoch); // even a suspend/resume between loop turns
	output(motors, 8); driver->reset(0); CHECK(!xinput::takeRumble(command));
	xinput::service(); CHECK(!xinput::setRumbleTarget(1, 18));
	boot(r); // consumed one-shot: next cold boot returns Steam, with config HID
	CHECK(usb::inputMode() == Mode::Steam && TinyUSBDevice.descriptors.size() == 5);
	uint8_t read[] = {0xfe,0}; transact(r, read, 2, reply);
	CHECK(reply[4] == 0 && reply[5] == input_mode::CONFIG_SIZE);
	uint8_t write[31] = {0xfe,29,1}; memcpy(write + 3, reply + 8, 4); memcpy(write + 7, reply + 12, 24);
	write[7 + 2 + 9] = input_mode::A;
	uint32_t previous = get32(reply + 8);
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 0 && saves == 1 && get32(reply + 8) != previous);
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 2 && saves == 1);
	memcpy(write + 3, reply + 8, 4); write[7 + 2 + 9] = input_mode::B; fail_save = true;
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 6 && reply[12 + 2 + 9] == input_mode::A);
	fail_save = false; write[9] = 0xff;
	transact(r, write, sizeof(write), reply); CHECK(reply[4] == 1);
	transact(r, enter, sizeof(enter), reply); settle(r);
	mock_power.USBREGSTATUS = 0; tud_event_hook_cb(0, DCD_EVENT_UNPLUGGED, true);
	mock_power.USBREGSTATUS = POWER_USBREGSTATUS_VBUSDETECT_Msk;
	settle(r); // externally powered unplug uses the same persisted restart path
	CHECK(usb::inputMode() == Mode::Steam && TinyUSBDevice.vid == 0x28de && TinyUSBDevice.pid == 0x1142);
	CHECK(Adafruit_USBD_HID::instances.size() == 5 && Adafruit_USBD_HID::instances[1]->interface_number == 1);
	CHECK(!xinput::send(report));
	CHECK(!xinput::osString());
	transact(r, read, 2, reply); CHECK(reply[12 + 2 + 9] == input_mode::A); // configuration survives mode reset
	// Persistence is explicit. Consumed one-shots are the default; enabling
	// the option instead keeps Xbox over the next power cycle.
	saved_config.flags |= input_mode::PERSIST_MODE;
	usb::configureInput(saved_config, persistConfig);
	transact(r, enter, sizeof(enter), reply); settle(r); boot(r);
	CHECK(usb::inputMode() == Mode::Xbox && TinyUSBDevice.pid == 0x028e);
	saved_config.flags &= uint8_t(~input_mode::PERSIST_MODE); boot(r);
	CHECK(usb::inputMode() == Mode::Steam && TinyUSBDevice.pid == 0x1142);
	TinyUSBDevice.unplug_on_detach = false;
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
}
}
void testPs5();
void testSwitch();
void testMotion();
void testLizard();
void testInputMode()
{
	testMapping(); testChord(); testGuide(); testDpadDirections(); testRecord(); testUsbMode();
	std::printf("PASS: input mapping, startup chord, config journal, XInput driver and USB mode lifecycle (%u assertions; software only)\n", assertions);
	testPs5();
	testSwitch();
	testMotion();
	testLizard();
}
