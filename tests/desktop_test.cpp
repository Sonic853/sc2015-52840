#include "usb.h"
#include <Adafruit_TinyUSB.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define DESKTOP_CHECK(value) do { if (!(value)) { \
	std::fprintf(stderr, "DESKTOP FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

namespace {

void drainDesktop(Adafruit_USBD_HID &hid)
{
	hid.completeReport();
	for (unsigned i = 0; i < 8; ++i) {
		size_t before = hid.sent.size();
		usb::pump();
		if (hid.sent.size() == before)
			return;
		DESKTOP_CHECK(hid.sent.size() == before + 1 && !hid.ready());
		hid.completeReport();
	}
	DESKTOP_CHECK(false); // Only neutral/snapshot reports may be drained here.
}

void resetDesktop(Adafruit_USBD_HID &hid)
{
	hid.protocol_mode = HID_PROTOCOL_REPORT;
	usb::desktopEnabled(false);
	drainDesktop(hid);
	usb::desktopEnabled(true);
	drainDesktop(hid);
	hid.sent.clear();
}

void checkNeutral(const std::vector<uint8_t> &report)
{
	DESKTOP_CHECK((report.size() == 9 && report[0] == 1) ||
		      (report.size() == 5 && report[0] == 2));
	for (size_t i = 1; i < report.size(); ++i)
		DESKTOP_CHECK(report[i] == 0);
}

void testFairness(Adafruit_USBD_HID &hid)
{
	resetDesktop(hid);
	unsigned keyboards = 0, mice = 0;
	for (unsigned frame = 0; frame < 24; ++frame) {
		uint8_t keys[8] = {};
		keys[2] = uint8_t(4 + (frame & 1));
		const uint8_t mouse[4] = {1, 3, 0, 0};
		usb::desktop(keys, mouse);
		size_t before = hid.sent.size();
		usb::pump();
		DESKTOP_CHECK(hid.sent.size() == before + 1 && !hid.ready());
		const auto &report = hid.sent.back();
		DESKTOP_CHECK(report[0] == ((frame & 1) ? 2 : 1));
		if (report[0] == 1) {
			++keyboards;
			DESKTOP_CHECK(report.size() == 9 && report[3] == keys[2]);
		} else {
			++mice;
			DESKTOP_CHECK(report.size() == 5 && report[1] == 1 && report[2] == 6);
		}
		// Neither the pump nor the strict mock may accept a second transfer
		// until the host completes this endpoint's outstanding report.
		usb::pump();
		DESKTOP_CHECK(hid.sent.size() == before + 1);
		const uint8_t rejected[4] = {};
		DESKTOP_CHECK(!hid.sendReport(2, rejected, sizeof(rejected)));
		DESKTOP_CHECK(hid.sent.size() == before + 1);
		hid.completeReport();
	}
	DESKTOP_CHECK(keyboards == 12 && mice == 12);
}

void testDisableAndReset(Adafruit_USBD_HID &hid)
{
	resetDesktop(hid);
	const uint8_t keys[8] = {2, 0, 4};
	const uint8_t mouse[4] = {1, 100, uint8_t(-50), 7};
	for (unsigned i = 0; i < 3; ++i)
		usb::desktop(keys, mouse);
	usb::pump(); // Keyboard in flight; mouse has more than one packet of motion.
	DESKTOP_CHECK(hid.sent.size() == 1 && hid.sent.back()[0] == 1);
	DESKTOP_CHECK(!hid.ready());
	usb::desktopEnabled(false);
	usb::desktop(keys, mouse); // A late snapshot cannot bypass the disabled gate.
	usb::pump();
	DESKTOP_CHECK(hid.sent.size() == 1);
	drainDesktop(hid);
	DESKTOP_CHECK(hid.sent.size() == 3);
	checkNeutral(hid.sent[1]);
	checkNeutral(hid.sent[2]);
	DESKTOP_CHECK(hid.sent[1][0] != hid.sent[2][0]);
	for (unsigned i = 0; i < 5; ++i) {
		usb::desktopEnabled(false);
		usb::desktop(keys, mouse);
		usb::pump();
		DESKTOP_CHECK(hid.sent.size() == 3);
	}

	// Receiver::feature(B1) may publish its snapshot before the loop copies
	// receiver.desktopEnabled(). Keep keys/buttons, discard disabled motion.
	usb::desktop(keys, mouse);
	usb::desktopEnabled(true);
	hid.sent.clear();
	drainDesktop(hid);
	DESKTOP_CHECK(hid.sent.size() == 2);
	DESKTOP_CHECK(hid.sent[0].size() == 9 && hid.sent[0][0] == 1);
	DESKTOP_CHECK(!std::memcmp(hid.sent[0].data() + 1, keys, sizeof(keys)));
	DESKTOP_CHECK(hid.sent[1] == std::vector<uint8_t>({2, 1, 0, 0, 0}));

	// The integration reset path also works without pumping between the
	// disable and restore calls: no old key/button/movement survives.
	usb::desktop(keys, mouse);
	usb::desktopEnabled(false);
	usb::desktopEnabled(true);
	hid.sent.clear();
	drainDesktop(hid);
	DESKTOP_CHECK(hid.sent.size() == 2);
	checkNeutral(hid.sent[0]);
	checkNeutral(hid.sent[1]);
}

void testProtocolModes(Adafruit_USBD_HID &hid)
{
	resetDesktop(hid);
	uint8_t keys[8] = {2, 0, 7};
	const uint8_t mouse[4] = {3, 100, 5, 1};
	usb::desktop(keys, mouse);
	usb::pump();
	DESKTOP_CHECK(hid.sent.size() == 1 && hid.sent.back().size() == 9);
	DESKTOP_CHECK(hid.sent.back()[0] == 1 && !hid.ready());

	hid.protocol_mode = HID_PROTOCOL_BOOT;
	usb::pump(); // The already submitted Report-format packet is not cancelled.
	DESKTOP_CHECK(hid.sent.size() == 1);
	hid.completeReport();
	usb::pump();
	DESKTOP_CHECK(hid.sent.size() == 2 && hid.sent.back().size() == 8);
	DESKTOP_CHECK(!std::memcmp(hid.sent.back().data(), keys, sizeof(keys)));
	hid.completeReport();
	for (unsigned i = 0; i < 4; ++i) {
		keys[2] = uint8_t(8 + i);
		const uint8_t boot_mouse[4] = {7, 127, 127, 9};
		usb::desktop(keys, boot_mouse);
		size_t before = hid.sent.size();
		usb::pump();
		DESKTOP_CHECK(hid.sent.size() == before + 1 && hid.sent.back().size() == 8);
		DESKTOP_CHECK(!std::memcmp(hid.sent.back().data(), keys, sizeof(keys)));
		hid.completeReport();
		usb::pump();
		DESKTOP_CHECK(hid.sent.size() == before + 1); // No Boot mouse packet.
	}

	// Returning to Report mode resends the unchanged keyboard in the new
	// format and the latest mouse buttons, without old/Boot-mode displacement.
	hid.sent.clear();
	hid.protocol_mode = HID_PROTOCOL_REPORT;
	drainDesktop(hid);
	DESKTOP_CHECK(hid.sent.size() == 2);
	DESKTOP_CHECK(hid.sent[0].size() == 9 && hid.sent[0][0] == 1);
	DESKTOP_CHECK(!std::memcmp(hid.sent[0].data() + 1, keys, sizeof(keys)));
	DESKTOP_CHECK(hid.sent[1] == std::vector<uint8_t>({2, 7, 0, 0, 0}));
	const uint8_t fresh_mouse[4] = {7, 9, uint8_t(-5), 2};
	usb::desktop(keys, fresh_mouse);
	usb::pump();
	DESKTOP_CHECK(hid.sent.size() == 3);
	DESKTOP_CHECK(hid.sent.back() == std::vector<uint8_t>({2, 7, 9, uint8_t(-5), 2}));
	hid.completeReport();

	// A fresh relative input can arrive after SET_PROTOCOL but before pump().
	// Leaving Boot must preserve that new Report-mode input, not flush it.
	hid.protocol_mode = HID_PROTOCOL_BOOT;
	drainDesktop(hid);
	hid.sent.clear();
	hid.protocol_mode = HID_PROTOCOL_REPORT;
	usb::desktop(keys, fresh_mouse);
	drainDesktop(hid);
	DESKTOP_CHECK(hid.sent.size() == 2 && hid.sent[0].size() == 9);
	DESKTOP_CHECK(hid.sent[0][0] == 1);
	DESKTOP_CHECK(hid.sent[1] == std::vector<uint8_t>({2, 7, 9, uint8_t(-5), 2}));

	// Disabled Boot mode permits a single eight-byte neutral keyboard only.
	usb::desktopEnabled(false);
	hid.protocol_mode = HID_PROTOCOL_BOOT;
	hid.sent.clear();
	drainDesktop(hid);
	DESKTOP_CHECK(hid.sent.size() == 1 && hid.sent[0] == std::vector<uint8_t>(8, 0));
	usb::desktopEnabled(false);
	usb::pump();
	DESKTOP_CHECK(hid.sent.size() == 1);
}

} // namespace

// Call after testUsb(): it initializes the production usb::hid array and
// registers all five interfaces. This test intentionally does not call begin().
void testDesktopUsb()
{
	DESKTOP_CHECK(Adafruit_USBD_HID::instances.size() == 5);
	DESKTOP_CHECK(TinyUSBDevice.mounted() && !TinyUSBDevice.suspended());
	auto &hid = *Adafruit_USBD_HID::instances[0];
	bool previous_strict = hid.strict_in_flight;
	uint8_t previous_protocol = hid.protocol_mode;
	hid.strict_in_flight = true;
	testFairness(hid);
	testDisableAndReset(hid);
	testProtocolModes(hid);
	resetDesktop(hid);
	hid.protocol_mode = previous_protocol;
	drainDesktop(hid);
	hid.sent.clear();
	hid.strict_in_flight = previous_strict;
	std::puts("PASS: desktop USB single-in-flight fairness, disable/reset release and standard Boot/Report formats");
}
