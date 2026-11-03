#include "protocol.h"
#include "radio_policy.h"
#include "usb.h"
#include "rumble.h"
#include "xinput.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <device/dcd.h>
#include <device/usbd_pvt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#endif

extern "C" void tud_event_hook_cb(uint8_t, uint32_t, bool);
extern "C" const usbd_class_driver_t *__wrap_usbd_app_driver_get_cb(uint8_t *);
static input_mode::BootState integration_boot = input_mode::defaultBootState();
static unsigned integration_resets;
static bool integrationSaveMode(input_mode::Mode mode)
{
	integration_boot = input_mode::bootRequest(input_mode::defaults(), mode); return true;
}
static void integrationReset() { ++integration_resets; }

namespace {

unsigned assertions;
const char *context = "queue USB";
#define QU_CHECK(value) do { ++assertions; if (!(value)) { \
	std::fprintf(stderr, "QUEUE USB FAILED [%s] %s:%d: %s\n", context, __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

using Page = std::array<uint8_t, 64>;
using Haptic = std::array<uint8_t, 10>;

uint32_t le32(const uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
	       (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void put32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}

bool aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
#ifdef _WIN32
	BCRYPT_ALG_HANDLE provider = nullptr;
	BCRYPT_KEY_HANDLE handle = nullptr;
	QU_CHECK(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0);
	QU_CHECK(BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
		ULONG(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0) == 0);
	QU_CHECK(BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0,
		const_cast<PUCHAR>(key), 16, 0) == 0);
	ULONG count = 0;
	bool ok = BCryptEncrypt(handle, const_cast<PUCHAR>(in), 16, nullptr,
		nullptr, 0, out, 16, &count, 0) == 0 && count == 16;
	BCryptDestroyKey(handle);
	BCryptCloseAlgorithmProvider(provider, 0);
	return ok;
#else
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	QU_CHECK(ctx);
	int count = 0;
	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, out, &count, in, 16) == 1 && count == 16;
	EVP_CIPHER_CTX_free(ctx);
	return ok;
#endif
}

unsigned random_byte;
bool randomBytes(uint8_t *data, size_t size)
{
	for (size_t i = 0; i < size; ++i) data[i] = uint8_t(++random_byte);
	return true;
}
bool persist(const sc::Database &) { QU_CHECK(false); return false; }
void input(unsigned, const uint8_t[64]) {}
const sc::Hooks hooks = {aes, randomBytes, persist, input, nullptr, ::micros};

sc::Database database()
{
	sc::Database db{};
	const uint8_t address[] = {0x28, 0x36, 0x57, 0x62, 0x94};
	std::memcpy(db.address, address, sizeof(address));
	for (unsigned slot = 0; slot < sc::SLOT_COUNT; ++slot) {
		auto &bond = db.bonds[slot];
		bond.used = true;
		for (unsigned i = 0; i < 8; ++i) bond.id[i] = uint8_t(0x30 + slot * 16 + i);
		for (unsigned i = 0; i < 16; ++i) bond.key[i] = uint8_t(0x90 + slot * 16 + i);
	}
	return db;
}

sc::QueueMetrics metrics(const sc::Receiver &receiver)
{
	sc::QueueMetrics result;
	receiver.copyQueueMetrics(result);
	return result;
}

const sc::QueueEvent &eventAt(const sc::QueueMetrics &m, unsigned index)
{
	QU_CHECK(index < m.event_count);
	return m.events[(m.event_head + 16 - m.event_count + index) % 16];
}

struct Fixture {
	sc::Receiver receiver;
	explicit Fixture(uint32_t at = 100000)
	{
		mock_micros = at;
		random_byte = 0;
		QU_CHECK(receiver.begin(database(), hooks));
		// testUsb already initialized the native mock. Its queues/statistics are
		// process-global, and its previous receiver_source may now be expired.
		QU_CHECK(Adafruit_USBD_HID::instances.size() == 5);
		tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
		QU_CHECK(usb::process(receiver) == 0);
		usb::publish(receiver);
		QU_CHECK(receiver.queueClockAvailable());
		auto m = metrics(receiver);
		QU_CHECK(m.event_count == 0 && m.event_head == 0 && m.next_event_id == 1);
		for (const auto &s : m.slots) {
			QU_CHECK(s.admitted == 0 && s.haptic_admitted == 0 && s.haptic_full == 0);
			for (auto n : s.a2_a6_full) QU_CHECK(n == 0);
			QU_CHECK(s.other_full == 0 && s.initial_retries == 0 && s.count_retries == 0);
			QU_CHECK(s.commit_waits == 0 && s.offline == 0 && s.serialized == 0 && s.retired == 0);
			QU_CHECK(s.cleared_fragments == 0 && s.max_fifo_age_us == 0 && s.max_lane_age_us == 0);
			QU_CHECK(s.fifo_high_water == 0);
		}
	}
};

struct Message {
	uint8_t type;
	std::vector<uint8_t> body;
	uint32_t at;
};

struct Peer {
	uint8_t key[16]{}, iv[8]{};
	unsigned pipe = 2;
	uint64_t up = 1, down = 0;
	std::vector<Message> messages;

	sc::Packet uplink() const
	{
		const uint8_t info[8] = {4};
		sc::Packet wire{};
		uint8_t header = uint8_t(1 | ((up & 7) << 4) | ((down & 1) << 2));
		QU_CHECK(sc::seal(aes, key, iv, up, false, header, info, sizeof(info), wire));
		return wire;
	}

	bool accept(const sc::Packet &wire)
	{
		if (wire.size <= 1 || ((wire.data[0] >> 2) & 1) == (down & 1)) return false;
		uint8_t plain[sc::MAX_PLAIN];
		unsigned size = 0;
		// Exactly the next counter, never a forward search to hide a lost pulse.
		if (!sc::open(aes, key, iv, down + 1, true, wire, plain, size)) return false;
		QU_CHECK((wire.data[0] & 3) != 0);
		messages.push_back({uint8_t(wire.data[0] & 3),
			std::vector<uint8_t>(plain, plain + size), ::micros()});
		++down;
		return true;
	}

	void feedback(const sc::Packet &wire)
	{
		// Controller P:2c7a..2cb6: rejected old ciphertext is NOT uplink
		// feedback, even if its unauthenticated upper-three-bit echo matches.
		if (!wire.size || (wire.size > 1 && !accept(wire))) return;
		if (((wire.data[0] >> 4) & 7) == (up & 7)) ++up;
	}
};

Peer restore(sc::Receiver &receiver, unsigned bond_index = 0)
{
	const auto &bond = receiver.database().bonds[bond_index];
	sc::Packet lookup{};
	lookup.size = 9;
	std::memcpy(lookup.data + 1, bond.id, 8);
	receiver.receive(1, lookup);
	auto ack = receiver.acknowledgement(1, 0);
	QU_CHECK(ack.size == 22);
	Peer peer;
	peer.pipe = ack.data[9] & 7;
	QU_CHECK(peer.pipe == bond_index + 2);
	std::memcpy(peer.key, bond.key, 16);
	std::memcpy(peer.iv, ack.data + 1, 4);
	for (unsigned i = 0; i < 4; ++i) peer.iv[4 + i] = uint8_t(0xa0 + bond_index * 4 + i);
	uint8_t plain[17];
	std::memcpy(plain, receiver.database().address, 5);
	std::memcpy(plain + 5, bond.id, 8);
	std::memcpy(plain + 13, peer.iv + 4, 4);
	sc::Packet join{};
	QU_CHECK(sc::seal(aes, peer.key, ack.data + 1, 0, false, 2, plain, sizeof(plain), join));
	receiver.receive(1, join);
	QU_CHECK(receiver.connected(peer.pipe - 2) && !receiver.pending(peer.pipe - 2));
	return peer;
}

void poll(sc::Receiver &receiver, Peer &peer)
{
	receiver.receive(peer.pipe, peer.uplink());
	peer.feedback(receiver.acknowledgement(peer.pipe, unsigned(peer.down & 1), true));
	receiver.service();
}

Page transport(sc::Receiver &receiver);

void drain(sc::Receiver &receiver, Peer &peer)
{
	receiver.service();
	for (unsigned i = 0; i < 256; ++i) {
		poll(receiver, peer);
		const auto state = transport(receiver);
		// Lifetime serialized/retired need not match after a session clears
		// occupied lanes. Inspect live FIFO/lane occupancy instead.
		if (state[34] == 0 && (state[35] & 3) == 0) {
			QU_CHECK(receiver.acknowledgement(peer.pipe, 0, true).size == 1);
			QU_CHECK(receiver.acknowledgement(peer.pipe, 1, true).size == 1);
			QU_CHECK(receiver.statistics().mic_failures == 0 && receiver.statistics().invalid == 0);
			return;
		}
	}
	QU_CHECK(false);
}

Peer ready(sc::Receiver &receiver)
{
	Peer peer = restore(receiver);
	drain(receiver, peer);
	QU_CHECK(peer.messages.size() == 2 && peer.down == 2);
	QU_CHECK(peer.messages[0].type == 1 && peer.messages[0].body.size() == 8 && peer.messages[0].body[0] == 4);
	QU_CHECK(peer.messages[1].type == 1 && peer.messages[1].body == std::vector<uint8_t>({3, 1}));
	peer.messages.clear();
	return peer;
}

Haptic pulse(unsigned i)
{
	return {{0x8f, 8, uint8_t(i & 1), uint8_t(i + 1), uint8_t(0x10 + (i >> 8)),
		0x90, 1, uint8_t(i + 2), 0, 0}};
}

bool write(unsigned slot, const uint8_t *data, unsigned size)
{
	return mockFeatureSet(slot + 1, data, size, size);
}

void writePulse(unsigned slot = 0, unsigned i = 0)
{
	auto p = pulse(i);
	QU_CHECK(write(slot, p.data(), unsigned(p.size())));
}

Adafruit_USBD_HID *hid(unsigned slot = 3)
{
	return Adafruit_USBD_HID::instances[slot + 1];
}

void checkHeader(const Page &p, unsigned page, unsigned status, uint32_t id, uint32_t at)
{
	QU_CHECK(p[0] == 0xfc && p[1] == 62 && p[2] == 1 && p[3] == page);
	QU_CHECK(p[4] == radio::FIRMWARE_REVISION && p[5] == status);
	QU_CHECK(le32(p.data() + 6) == id && le32(p.data() + 10) == at);
	if (status) for (unsigned i = 14; i < 64; ++i) QU_CHECK(p[i] == 0);
}

Page readPublished(sc::Receiver &receiver, unsigned slot = 3)
{
	Page p;
	p.fill(0xcd);
	QU_CHECK(hid(slot)->get(0, HID_REPORT_TYPE_FEATURE, p.data(), 64) == 64);
	QU_CHECK(usb::process(receiver) == 1); // The completed GET owns one consume record.
	usb::publish(receiver);
	return p;
}

Page request(sc::Receiver &receiver, const uint8_t *data, unsigned size, unsigned slot = 3)
{
	QU_CHECK(write(slot, data, size));
	QU_CHECK(usb::process(receiver) == 1);
	usb::publish(receiver);
	return readPublished(receiver, slot);
}

Page pageRequest(sc::Receiver &receiver, unsigned page, uint32_t token, unsigned slot = 3)
{
	uint8_t selector[] = {0xfc, 5, uint8_t(page), 0, 0, 0, 0};
	put32(selector + 3, token);
	return request(receiver, selector, sizeof(selector), slot);
}

struct Snapshot {
	std::array<Page, 18> pages{};
	uint32_t id() const { return le32(pages[0].data() + 6); }
	uint32_t at() const { return le32(pages[0].data() + 10); }
	uint32_t rf(unsigned slot, unsigned field) const { return le32(pages[1 + slot].data() + 14 + 4 * field); }
	uint32_t detail(unsigned slot, unsigned field) const { return le32(pages[5 + slot].data() + 14 + 4 * field); }
	uint32_t extra(unsigned slot, unsigned field) const { return le32(pages[17].data() + 14 + 12 * slot + 4 * field); }
	uint8_t pending(unsigned slot) const { return pages[5 + slot][63]; }
};

Snapshot readSnapshot(sc::Receiver &receiver, const Page &capture)
{
	Snapshot s;
	s.pages[0] = capture;
	QU_CHECK(s.id() != 0);
	checkHeader(capture, 0, 0, s.id(), s.at());
	for (unsigned page = 1; page <= 17; ++page) {
		s.pages[page] = pageRequest(receiver, page, s.id());
		checkHeader(s.pages[page], page, 0, s.id(), s.at());
	}
	return s;
}

Snapshot capture(sc::Receiver &receiver)
{
	const uint8_t selector[] = {0xfc, 0};
	return readSnapshot(receiver, request(receiver, selector, sizeof(selector)));
}

uint32_t delta(uint32_t after, uint32_t before) { return after - before; }

void conservation(const Snapshot &before, const Snapshot &after)
{
	for (unsigned slot = 0; slot < 4; ++slot) {
		uint64_t received = delta(after.detail(slot, 8), before.detail(slot, 8));
		uint64_t outcomes = uint64_t(delta(after.rf(slot, 1), before.rf(slot, 1))) +
			delta(after.rf(slot, 2), before.rf(slot, 2)) +
			delta(after.detail(slot, 11), before.detail(slot, 11)) +
			delta(after.extra(slot, 1), before.extra(slot, 1)) +
			delta(after.extra(slot, 2), before.extra(slot, 2));
		// USB timeouts are a subset of RF haptic_full, not a second loss.
		QU_CHECK(received + before.pending(slot) == outcomes + after.pending(slot));
		QU_CHECK(delta(after.detail(slot, 10), before.detail(slot, 10)) <=
			delta(after.rf(slot, 2), before.rf(slot, 2)));
	}
}

void fillRf(sc::Receiver &receiver)
{
	const uint8_t setting[] = {0x87, 0};
	for (unsigned i = 0; i < 12; ++i) QU_CHECK(receiver.feature(0, setting, sizeof(setting)));
	receiver.service();
	for (unsigned i = 0; i < 2; ++i) QU_CHECK(receiver.feature(0, setting, sizeof(setting)));
	auto p = pulse(0);
	QU_CHECK(!receiver.canQueueFeature(0, p.data(), unsigned(p.size())));
}

Page transport(sc::Receiver &receiver)
{
	// FB exposes the low TX counter and lane metadata without private access.
	const uint8_t selector[] = {0xfb, 0};
	QU_CHECK(receiver.feature(2, selector, sizeof(selector)));
	Page p{};
	uint32_t version;
	receiver.peekFeature(2, p.data(), version);
	QU_CHECK(p[0] == 0xfb && (p[60] & 1));
	return p;
}

void testWait(bool free_before_deadline, bool wrap)
{
	context = free_before_deadline ? "haptic capacity before deadline" : "haptic deadline rejection";
	Fixture f(wrap ? 0xfffff000u : 100000u);
	auto &r = f.receiver;
	Peer peer = ready(r);
	fillRf(r);
	auto baseline = capture(r);
	uint32_t start = ::micros();
	auto initial = metrics(r);
	auto tx = transport(r);
	writePulse();
	for (uint32_t elapsed : {0u, 1u, 1000u, 4000u, 7999u}) {
		mock_micros = start + elapsed;
		QU_CHECK(usb::process(r) == 0);
		auto m = metrics(r);
		QU_CHECK(m.slots[0].haptic_admitted == 0 && m.slots[0].haptic_full == 0);
		QU_CHECK(m.slots[0].serialized == initial.slots[0].serialized);
		QU_CHECK(m.event_count == initial.event_count + 1);
		QU_CHECK(eventAt(m, m.event_count - 1).kind == uint8_t(sc::QueueEventKind::UsbWait));
		QU_CHECK(r.statistics().queue_full == 0);
	}
	QU_CHECK(transport(r)[37] == tx[37]);
	if (free_before_deadline) {
		for (unsigned i = 0; i < 2; ++i) poll(r, peer);
		auto p = pulse(0);
		QU_CHECK(r.canQueueFeature(0, p.data(), unsigned(p.size())));
		QU_CHECK(usb::process(r) == 1);
		QU_CHECK(metrics(r).slots[0].haptic_admitted == 1 && metrics(r).slots[0].haptic_full == 0);
	} else {
		mock_micros = start + 8000;
		QU_CHECK(usb::process(r) == 1);
		auto m = metrics(r);
		QU_CHECK(m.slots[0].haptic_admitted == 0 && m.slots[0].haptic_full == 1);
		QU_CHECK(m.slots[0].serialized == initial.slots[0].serialized);
		QU_CHECK(transport(r)[37] == tx[37]);
		QU_CHECK(m.event_count == initial.event_count + 3 && r.statistics().queue_full == 1);
		const auto &timeout = eventAt(m, m.event_count - 2);
		QU_CHECK(timeout.kind == uint8_t(sc::QueueEventKind::UsbTimeout) && timeout.age_us == 8000);
		QU_CHECK(timeout.command == 0x8f && timeout.length == 10 && timeout.needed == 1 && timeout.usb_depth == 1);
		QU_CHECK(eventAt(m, m.event_count - 1).kind == uint8_t(sc::QueueEventKind::HapticFull));
	}
	for (unsigned i = 0; i < 12; ++i) QU_CHECK(usb::process(r) == 0);
	auto after = capture(r);
	conservation(baseline, after);
	QU_CHECK(delta(after.detail(0, 8), baseline.detail(0, 8)) == 1);
	QU_CHECK(delta(after.detail(0, 9), baseline.detail(0, 9)) == 1);
	QU_CHECK(delta(after.detail(0, 10), baseline.detail(0, 10)) == unsigned(!free_before_deadline));
	QU_CHECK(after.pending(0) == 0);
	drain(r, peer);
	QU_CHECK(peer.messages.size() == 14 + unsigned(free_before_deadline));
	for (unsigned i = 0; i < 14; ++i)
		QU_CHECK(peer.messages[i].type == 3 && peer.messages[i].body == std::vector<uint8_t>({0x87, 0}));
	if (free_before_deadline) {
		auto p = pulse(0);
		QU_CHECK(peer.messages.back().body == std::vector<uint8_t>(p.begin(), p.end()));
	}
	// A subsequent accepted command must use the very next CCM counter.
	writePulse(0, 99);
	QU_CHECK(usb::process(r) == 1);
	drain(r, peer);
	auto sentinel = pulse(99);
	QU_CHECK(peer.messages.back().body == std::vector<uint8_t>(sentinel.begin(), sentinel.end()));
	QU_CHECK(peer.down == 2 + peer.messages.size());
}

void testCancellationAndOffline()
{
	context = "stale haptic before deadline";
	{
		Fixture f;
		auto &r = f.receiver;
		Peer peer = ready(r);
		fillRf(r);
		auto before = capture(r);
		writePulse();
		QU_CHECK(usb::process(r) == 0);
		mock_micros += 1000;
		uint32_t generation = r.sessionGeneration(0);
		r.disconnect(0);
		peer = restore(r);
		QU_CHECK(r.sessionGeneration(0) != generation);
		QU_CHECK(usb::process(r) == 1);
		for (unsigned i = 0; i < 4; ++i) QU_CHECK(usb::process(r) == 0);
		auto after = capture(r);
		conservation(before, after);
		QU_CHECK(delta(after.detail(0, 11), before.detail(0, 11)) == 1);
		QU_CHECK(delta(after.detail(0, 10), before.detail(0, 10)) == 0);
		QU_CHECK(delta(after.extra(0, 1), before.extra(0, 1)) == 0);
		QU_CHECK(after.rf(0, 1) == 0 && after.rf(0, 2) == 0);
		auto m = metrics(r);
		QU_CHECK(eventAt(m, m.event_count - 1).kind == uint8_t(sc::QueueEventKind::StaleUsb));
		drain(r, peer);
		QU_CHECK(peer.messages.size() == 2); // Only the restored session's controls.
	}
	context = "bus reset cancels pending haptics once";
	{
		Fixture f;
		auto &r = f.receiver;
		Peer peer = ready(r);
		fillRf(r);
		auto before = capture(r);
		for (unsigned i = 0; i < 3; ++i) writePulse(0, i);
		QU_CHECK(usb::process(r) == 0);
		mock_micros += 2000;
		tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
		auto p = pulse(4);
		QU_CHECK(!write(0, p.data(), unsigned(p.size())));
		QU_CHECK(usb::process(r) == 0);
		for (unsigned i = 0; i < 4; ++i) QU_CHECK(usb::process(r) == 0);
		QU_CHECK(r.connected(0));
		auto after = capture(r);
		conservation(before, after);
		QU_CHECK(delta(after.detail(0, 8), before.detail(0, 8)) == 3);
		QU_CHECK(delta(after.extra(0, 2), before.extra(0, 2)) == 3);
		QU_CHECK(delta(after.detail(0, 7), before.detail(0, 7)) == 0);
		QU_CHECK(delta(after.detail(0, 10), before.detail(0, 10)) == 0);
		QU_CHECK(after.rf(0, 1) == 0 && after.rf(0, 2) == 0 && after.pending(0) == 0);
		drain(r, peer);
		QU_CHECK(peer.messages.size() == 14);
	}
	context = "offline and pending per-slot conservation";
	{
		Fixture f;
		auto &r = f.receiver;
		auto before = capture(r);
		// Capture is ahead of the haptics in the USB ring, but all their SET
		// status stages have completed when the snapshot is actually taken.
		const uint8_t selector[] = {0xfc, 0};
		QU_CHECK(write(3, selector, sizeof(selector)));
		writePulse(0, 0);
		writePulse(0, 1);
		writePulse(1, 2);
		QU_CHECK(usb::process(r, 1) == 1);
		usb::publish(r);
		Page frozen{};
		QU_CHECK(hid()->get(0, HID_REPORT_TYPE_FEATURE, frozen.data(), 64) == 64);
		QU_CHECK(usb::process(r) == 4); // Three offline writes plus the GET.
		usb::publish(r);
		auto pending = readSnapshot(r, frozen);
		conservation(before, pending);
		QU_CHECK(pending.pending(0) == 2 && pending.pending(1) == 1);
		QU_CHECK(pending.pages[17][62] == 4); // Includes the capture SET itself.
		auto after = capture(r);
		conservation(before, after);
		conservation(pending, after);
		for (unsigned slot = 0; slot < 2; ++slot) {
			QU_CHECK(delta(after.extra(slot, 1), before.extra(slot, 1)) == 2 - slot);
			QU_CHECK(delta(after.detail(slot, 0), before.detail(slot, 0)) == 2 - slot);
			QU_CHECK(after.pending(slot) == 0 && after.rf(slot, 1) == 0 && after.rf(slot, 2) == 0);
		}
		// begin() resets core metrics, whereas the USB wrapper's lifetime
		// counters survive both Receiver replacement and bus reset.
		QU_CHECK(r.begin(database(), hooks));
		tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
		QU_CHECK(usb::process(r) == 0);
		usb::publish(r);
		auto restarted = capture(r);
		QU_CHECK(metrics(r).event_count == 0 && metrics(r).next_event_id == 1);
		for (unsigned slot = 0; slot < 3; ++slot) {
			for (unsigned field = 0; field < 12; ++field) QU_CHECK(restarted.rf(slot, field) == 0);
			for (unsigned field = 0; field < 6; ++field) QU_CHECK(restarted.detail(slot, field) == 0);
			for (unsigned field = 6; field < 12; ++field)
				QU_CHECK(restarted.detail(slot, field) == after.detail(slot, field));
			for (unsigned field = 0; field < 3; ++field)
				QU_CHECK(restarted.extra(slot, field) == after.extra(slot, field));
		}
	}
}

void testBestEffortAndSettings()
{
	context = "A2-A6 immediate best effort and whole settings wait";
	Fixture f;
	auto &r = f.receiver;
	Peer peer = ready(r);
	fillRf(r);
	auto before = capture(r);
	for (uint8_t command = 0xa2; command <= 0xa6; ++command) {
		const uint8_t data[] = {command, 0};
		QU_CHECK(write(0, data, sizeof(data)));
	}
	QU_CHECK(usb::process(r) == 4);
	QU_CHECK(usb::process(r) == 1);
	QU_CHECK(usb::process(r) == 0);
	auto m = metrics(r);
	for (auto count : m.slots[0].a2_a6_full) QU_CHECK(count == 1);
	QU_CHECK(m.event_count == 5 && r.statistics().queue_full == 5);
	for (unsigned i = 0; i < 5; ++i)
		QU_CHECK(eventAt(m, i).kind == uint8_t(sc::QueueEventKind::BestEffortFull));
	Page setting{};
	setting[0] = 0x87; setting[1] = 62;
	for (unsigned i = 2; i < 64; ++i) setting[i] = uint8_t(0x40 + i);
	QU_CHECK(write(0, setting.data(), unsigned(setting.size())));
	for (uint32_t age : {0u, 7999u, 8000u, 25000u}) {
		mock_micros = 100000 + age;
		QU_CHECK(usb::process(r) == 0);
		QU_CHECK(metrics(r).slots[0].other_full == 0);
		QU_CHECK(metrics(r).event_count == 6);
	}
	// Free two FIFO records: a three-fragment setting still must wait intact.
	for (unsigned i = 0; i < 3; ++i) poll(r, peer);
	QU_CHECK(transport(r)[34] == 10);
	QU_CHECK(usb::process(r) == 0);
	QU_CHECK(!r.canQueueFeature(0, setting.data(), unsigned(setting.size())));
	poll(r, peer);
	QU_CHECK(r.canQueueFeature(0, setting.data(), unsigned(setting.size())));
	QU_CHECK(usb::process(r) == 1);
	QU_CHECK(usb::process(r) == 0);
	drain(r, peer);
	QU_CHECK(peer.messages.size() == 17);
	std::vector<uint8_t> delivered;
	for (unsigned i = 14; i < 17; ++i) {
		QU_CHECK(peer.messages[i].type == (i == 16 ? 3 : 2));
		delivered.insert(delivered.end(), peer.messages[i].body.begin(), peer.messages[i].body.end());
	}
	QU_CHECK(delivered == std::vector<uint8_t>(setting.begin(), setting.end()));
	auto after = capture(r);
	conservation(before, after);
	QU_CHECK(after.rf(0, 0) == 15 && after.rf(0, 8) == 0);
	QU_CHECK(delta(after.detail(0, 9), before.detail(0, 9)) == 0);
	QU_CHECK(delta(after.detail(0, 10), before.detail(0, 10)) == 0);
}

void checkRfPages(const Snapshot &s, const sc::QueueMetrics &rf)
{
	for (unsigned slot = 0; slot < 4; ++slot) {
		const auto &r = rf.slots[slot];
		const uint32_t admission[] = {r.admitted, r.haptic_admitted, r.haptic_full,
			r.a2_a6_full[0], r.a2_a6_full[1], r.a2_a6_full[2], r.a2_a6_full[3], r.a2_a6_full[4],
			r.other_full, r.initial_retries, r.count_retries, r.commit_waits};
		const uint32_t detail[] = {r.offline, r.serialized, r.retired, r.cleared_fragments,
			r.max_fifo_age_us, r.max_lane_age_us};
		for (unsigned i = 0; i < 12; ++i) QU_CHECK(s.rf(slot, i) == admission[i]);
		for (unsigned i = 0; i < 6; ++i) QU_CHECK(s.detail(slot, i) == detail[i]);
		QU_CHECK(s.pages[1 + slot][62] == r.fifo_high_water && s.pages[5 + slot][62] == r.fifo_high_water);
		QU_CHECK(s.pages[1 + slot][63] == s.pending(slot));
	}
	for (unsigned page = 9; page <= 16; ++page) {
		Page expected{};
		std::copy(s.pages[0].begin(), s.pages[0].begin() + 14, expected.begin());
		expected[3] = uint8_t(page);
		for (unsigned row = 0; row < 2; ++row) {
			unsigned index = (page - 9) * 2 + row;
			if (index >= rf.event_count) break;
			const auto &e = eventAt(rf, index);
			uint8_t *p = expected.data() + 14 + row * 24;
			put32(p, e.id); put32(p + 4, e.at_us); put32(p + 8, e.generation); put32(p + 12, e.age_us);
			const uint8_t metadata[] = {e.slot, e.kind, e.command, e.length, e.needed, e.fifo_count, e.lane_flags, e.usb_depth};
			std::memcpy(p + 16, metadata, sizeof(metadata));
			++expected[62];
		}
		QU_CHECK(s.pages[page] == expected); // Includes every reserved/padding byte.
	}
	uint32_t totals[8]{};
	uint8_t high = 0;
	for (unsigned slot = 0; slot < 4; ++slot) {
		totals[0] += s.rf(slot, 1); totals[1] += s.rf(slot, 2);
		for (unsigned i = 3; i < 8; ++i) totals[2] += s.rf(slot, i);
		totals[3] += s.rf(slot, 9) + s.rf(slot, 10);
		totals[4] += s.detail(slot, 9); totals[5] += s.detail(slot, 10);
		totals[6] = std::max(totals[6], s.detail(slot, 4));
		totals[7] = std::max(totals[7], s.extra(slot, 0));
		high = std::max(high, s.pages[1 + slot][62]);
	}
	for (unsigned i = 0; i < 8; ++i) QU_CHECK(le32(s.pages[0].data() + 18 + 4 * i) == totals[i]);
	QU_CHECK(s.pages[0][14] == 12 && s.pages[0][15] == 31 && s.pages[0][16] == rf.event_count);
	QU_CHECK(s.pages[0][17] <= 31 && s.pages[0][62] == high && s.pages[0][63] == 1);
	QU_CHECK(le32(s.pages[0].data() + 58) == 8000);
	QU_CHECK(s.pages[17][63] == 4);
}

void testFcCodec()
{
	context = "FC production codec and frozen metadata";
	Fixture f;
	auto &r = f.receiver;
	Peer peer = ready(r);
	auto baseline = capture(r);
	// Also exercise zero-entry event pages and their all-zero row padding.
	checkRfPages(baseline, metrics(r));
	for (unsigned i = 0; i < 3; ++i)
		r.queueEvent(i, sc::QueueEventKind::UsbWait, 0x87, 64, 3, i * 100, uint8_t(i + 1));
	baseline = capture(r);
	checkRfPages(baseline, metrics(r)); // Odd final event row and unused pages.
	Page payload{};
	payload[0] = 0x87; payload[1] = 62;
	for (unsigned i = 2; i < 64; ++i) payload[i] = uint8_t(0xe0 ^ (i * 7));
	QU_CHECK(r.feature(0, payload.data(), unsigned(payload.size())));
	r.service(); // Two payload fragments in lanes, one in the FIFO.
	const uint8_t setting[] = {0x87, 0};
	for (unsigned i = 0; i < 11; ++i) QU_CHECK(r.feature(0, setting, sizeof(setting)));
	writePulse();
	QU_CHECK(usb::process(r) == 0);
	mock_micros += 8000;
	QU_CHECK(usb::process(r) == 1);
	writePulse(1, 1); writePulse(2, 2);
	QU_CHECK(usb::process(r) == 2);
	for (unsigned i = 0; i < 21; ++i) {
		++mock_micros;
		r.queueEvent(i % 4, sc::QueueEventKind::UsbWait, uint8_t(0xa2 + i % 5),
			uint8_t(2 + i), uint8_t(1 + i % 3), 100 + 7 * i, uint8_t(i % 31));
	}
	usb::observeRxLatency(12345);
	const auto frozen_rf = metrics(r);
	QU_CHECK(frozen_rf.event_count == 16 && eventAt(frozen_rf, 0).id > 1);
	const uint8_t selector[] = {0xfc, 1, 0};
	QU_CHECK(write(3, selector, sizeof(selector)));
	QU_CHECK(usb::process(r) == 1);
	usb::publish(r);
	Page capture_page{};
	uint32_t version_before, version_after;
	r.peekFeature(3, capture_page.data(), version_before);
	// No status-consumption record for zero/short GETs, even after many probes.
	std::vector<Page> probes;
	const unsigned lengths[] = {0, 1, 2, 14, 63};
	for (unsigned length : lengths) {
		Page probe; probe.fill(0xcd);
		QU_CHECK(hid()->get(0, HID_REPORT_TYPE_FEATURE, probe.data(), uint16_t(length)) == length);
		for (unsigned i = length; i < 64; ++i) QU_CHECK(probe[i] == 0xcd);
		QU_CHECK(usb::process(r) == 0);
		r.peekFeature(3, capture_page.data(), version_after);
		QU_CHECK(version_after == version_before);
		probes.push_back(probe);
	}
	capture_page = readPublished(r);
	for (unsigned i = 0; i < probes.size(); ++i)
		QU_CHECK(std::equal(probes[i].begin(), probes[i].begin() + lengths[i], capture_page.begin()));
	uint32_t token = le32(capture_page.data() + 6), at = le32(capture_page.data() + 10);
	QU_CHECK(at == ::micros());
	QU_CHECK(le32(capture_page.data() + 50) >= 12345);
	// Change both RF and USB live state before fetching any selected page.
	mock_micros += 321;
	poll(r, peer); poll(r, peer);
	writePulse(0, 8); writePulse(1, 9);
	QU_CHECK(usb::process(r) == 2);
	r.queueEvent(0, sc::QueueEventKind::OtherFull, 0x87, 64, 3, 999, 7);
	auto frozen = readSnapshot(r, capture_page);
	checkRfPages(frozen, frozen_rf);
	conservation(baseline, frozen);
	QU_CHECK(delta(frozen.detail(0, 8), baseline.detail(0, 8)) == 1);
	QU_CHECK(delta(frozen.detail(0, 9), baseline.detail(0, 9)) == 1);
	QU_CHECK(delta(frozen.detail(0, 10), baseline.detail(0, 10)) == 1);
	for (unsigned slot = 0; slot < 3; ++slot) {
		QU_CHECK(delta(frozen.detail(slot, 6), baseline.detail(slot, 6)) == 1);
		QU_CHECK(delta(frozen.detail(slot, 7), baseline.detail(slot, 7)) == 1);
		QU_CHECK(delta(frozen.extra(slot, 1), baseline.extra(slot, 1)) == unsigned(slot != 0));
		QU_CHECK(delta(frozen.extra(slot, 2), baseline.extra(slot, 2)) == 0);
		QU_CHECK(frozen.pending(slot) == 0);
	}
	QU_CHECK(frozen.pages[17][62] == 1);
	for (unsigned page = 1; page <= 17; ++page) {
		mock_micros += 17;
		r.queueEvent(1, sc::QueueEventKind::Offline, 0x8f, 10, 1);
		QU_CHECK(pageRequest(r, page, token, page % 4) == frozen.pages[page]);
	}
	// All returned data is the specified counters/metadata. Synthetic bond
	// secrets must never appear in FC (including error responses).
	for (const auto &p : frozen.pages) {
		QU_CHECK(std::search(p.begin(), p.end(), payload.begin() + 2, payload.begin() + 18) == p.end());
		for (const auto &bond : r.database().bonds)
			if (bond.used)
				QU_CHECK(std::search(p.begin(), p.end(), bond.key, bond.key + 16) == p.end());
	}
	const uint8_t new_capture[] = {0xfc, 0};
	auto next = request(r, new_capture, sizeof(new_capture), 1);
	uint32_t next_id = le32(next.data() + 6), next_at = le32(next.data() + 10);
	QU_CHECK(next_id != token && next_at != at);
	checkHeader(pageRequest(r, 1, token), 1, 2, next_id, next_at);
	checkHeader(pageRequest(r, 17, 0), 17, 2, next_id, next_at);
	const std::vector<std::vector<uint8_t>> invalid = {
		{0xfc, 1, 1}, {0xfc, 2, 0, 0}, {0xfc, 4, 1, 0, 0, 0},
		{0xfc, 6, 1, 0, 0, 0, 0, 0}, {0xfc, 5, 18, 0, 0, 0, 0},
		{0xfc, 1, 255}
	};
	for (const auto &bad : invalid)
		checkHeader(request(r, bad.data(), unsigned(bad.size())), bad[2], 1, next_id, next_at);
	checkHeader(pageRequest(r, 17, next_id), 17, 0, next_id, next_at);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	QU_CHECK(usb::process(r) == 0);
	usb::publish(r);
	checkHeader(pageRequest(r, 1, next_id), 1, 2, next_id, next_at);
}

struct Arrival { uint32_t at; Haptic body; };
struct Trace { const char *name; std::vector<Arrival> arrivals; };

uint32_t read32(FILE *file)
{
	uint8_t bytes[4];
	QU_CHECK(std::fread(bytes, 1, 4, file) == 4);
	return le32(bytes);
}

std::vector<Trace> readTraces(const char *path)
{
	context = "captured haptic fixture format";
	QU_CHECK(path && *path);
	FILE *file = nullptr;
#ifdef _MSC_VER
	QU_CHECK(fopen_s(&file, path, "rb") == 0);
#else
	file = std::fopen(path, "rb");
#endif
	QU_CHECK(file);
	uint32_t count = read32(file);
	QU_CHECK(count > 0 && count <= 256);
	std::vector<Haptic> variants(count);
	for (auto &v : variants) {
		QU_CHECK(std::fread(v.data(), 1, v.size(), file) == v.size());
		QU_CHECK(v[0] == 0x8f && v[1] == 8);
	}
	QU_CHECK(read32(file) == 2);
	std::vector<Trace> traces = {{"original-278", {}}, {"XIAO-371", {}}};
	for (unsigned c = 0; c < traces.size(); ++c) {
		uint32_t n = read32(file);
		QU_CHECK(n == (c == 0 ? 278u : 371u));
		for (uint32_t i = 0; i < n; ++i) {
			uint32_t at = read32(file);
			int variant = std::fgetc(file);
			QU_CHECK(variant >= 0 && unsigned(variant) < variants.size());
			QU_CHECK(at <= 5000000 && (i == 0 || at >= traces[c].arrivals.back().at));
			traces[c].arrivals.push_back({at, variants[unsigned(variant)]});
		}
	}
	QU_CHECK(std::fgetc(file) == EOF && !std::ferror(file));
	QU_CHECK(std::fclose(file) == 0);
	return traces;
}

// Only the ISR/main boundary and arrival schedule are modeled. Production
// CCM, Receiver, ACK retention/phase gates, Feature wrappers and metrics run
// unchanged in both policies. The ACK is copied from a previous publication;
// receive()/authentication is delayed by 1500 us and never happens in arrive().
class DeferredRadio {
	struct Rx { sc::Packet wire; uint32_t at, ready; };
	sc::Receiver &receiver;
	radio::ResponseSnapshot published{};
	std::deque<Rx> queue;
	uint32_t start;

public:
	unsigned dropped = 0;
	uint32_t max_rx_age = 0;
	DeferredRadio(sc::Receiver &r, uint32_t base) : receiver(r), start(base) { publish(); }
	void publish()
	{
		for (unsigned pipe = 0; pipe < 6; ++pipe)
			for (unsigned parity = 0; parity < 2; ++parity)
				published.acknowledgements[pipe][parity] = receiver.acknowledgement(pipe, parity, true);
		for (unsigned i = 0; i < sc::SLOT_COUNT; ++i) published.joins[i] = receiver.joinReplay(i);
		published.mask = receiver.activePipes();
	}
	void arrive(Peer &peer, uint32_t relative)
	{
		sc::Packet wire = peer.uplink();
		QU_CHECK(published.mask & (1u << peer.pipe));
		bool retained = queue.size() < 31;
		if (retained) queue.push_back({wire, relative, relative + 1500});
		else ++dropped;
		if (radio::shouldAck(true, retained, 1))
			peer.feedback(published.select(peer.pipe, (wire.data[0] >> 2) & 1, wire.data, wire.size));
	}
	void drain(unsigned budget, uint32_t relative)
	{
		while (budget-- && !queue.empty() && queue.front().ready <= relative) {
			auto item = queue.front(); queue.pop_front();
			uint32_t age = ::micros() - (start + item.at);
			max_rx_age = std::max(max_rx_age, age);
			usb::observeRxLatency(age);
			receiver.advanceToHop(item.at / 1500);
			receiver.receive(2, item.wire);
		}
		// Do not age beyond retained, not-yet-authenticated input.
		if (queue.empty()) receiver.advanceToHop(relative / 1500);
	}
};

struct ReplayResult {
	unsigned admitted = 0, dropped = 0, usb_rejected = 0, initial_admitted = 0;
	unsigned rf_drop = 0, rf_high = 0, usb_high = 0;
	uint32_t max_rf_age = 0, max_usb_age = 0, rx_age = 0, p50 = 0, p95 = 0, maximum = 0;
	std::vector<size_t> admitted_indices;
	std::vector<uint32_t> latencies;
};

ReplayResult replay(const Trace &trace, uint32_t poll_us, bool newer)
{
	context = newer ? "new interleaved policy replay" : "old USB-first policy replay";
	Fixture f(200000);
	auto &r = f.receiver;
	Peer peer = ready(r);
	auto before = capture(r);
	uint32_t start = ::micros();
	r.resetHopClock();
	DeferredRadio radio(r, start);
	ReplayResult result;
	std::deque<size_t> pending;
	std::vector<size_t> usb_accepted;
	std::vector<bool> admitted(trace.arrivals.size(), false);
	size_t host = 0;
	uint32_t next_poll = 0, next_loop = 0;
	const uint32_t deadline = trace.arrivals.back().at + 1000000;
	const auto usbStep = [&](unsigned budget, uint32_t wait) {
		if (!pending.empty())
			result.max_usb_age = std::max(result.max_usb_age, uint32_t(::micros() - start - trace.arrivals[pending.front()].at));
		auto prior = metrics(r).slots[0];
		unsigned processed = usb::process(r, budget, wait);
		auto after = metrics(r).slots[0];
		unsigned accepted = delta(after.haptic_admitted, prior.haptic_admitted);
		unsigned dropped = delta(after.haptic_full, prior.haptic_full);
		QU_CHECK(processed == accepted + dropped && processed <= pending.size());
		// No receive/service inside this call: available capacity admits a
		// prefix, then full-FIFO timeouts retire a suffix (or leave it pending).
		for (unsigned i = 0; i < processed; ++i) {
			size_t index = pending.front(); pending.pop_front();
			result.max_usb_age = std::max(result.max_usb_age, uint32_t(::micros() - start - trace.arrivals[index].at));
			if (i < accepted) {
				QU_CHECK(!admitted[index]);
				admitted[index] = true;
				result.admitted_indices.push_back(index);
			}
		}
		result.admitted += accepted;
		result.dropped += dropped;
	};
	for (;;) {
		uint32_t next_host = host < trace.arrivals.size() ? trace.arrivals[host].at : deadline + 1;
		uint32_t at = std::min(next_host, std::min(next_poll, next_loop));
		if (at > deadline) break;
		mock_micros = start + at;
		// Explicit tie order: all host arrivals, RF arrival, then main loop.
		while (host < trace.arrivals.size() && trace.arrivals[host].at == at) {
			const auto &body = trace.arrivals[host].body;
			if (write(0, body.data(), unsigned(body.size()))) {
				pending.push_back(host);
				usb_accepted.push_back(host);
				result.usb_high = std::max(result.usb_high, unsigned(pending.size()));
			} else ++result.usb_rejected; // One attempt, no implicit host retry.
			++host;
		}
		if (next_poll == at) {
			radio.arrive(peer, at);
			next_poll += poll_us;
		}
		if (next_loop == at) {
			if (!newer) {
				usbStep(16, 0);
				radio.drain(32, at);
				r.service(); radio.publish();
			} else {
				for (unsigned round = 0; round < 4; ++round) {
					radio.drain(8, at);
					r.service(); radio.publish();
					usbStep(4, 8000);
					r.service(); radio.publish();
				}
			}
			usb::publish(r);
			next_loop += 1000;
			if (at == 0) result.initial_admitted = result.admitted;
		}
		auto m = metrics(r).slots[0];
		result.max_rf_age = std::max(result.max_rf_age, std::max(m.max_fifo_age_us, m.max_lane_age_us));
		QU_CHECK(r.connected(0));
	}
	mock_micros = start + deadline;
	QU_CHECK(host == trace.arrivals.size() && pending.empty());
	QU_CHECK(r.connected(0) && peer.up > 10);
	QU_CHECK(r.statistics().invalid == 0 && r.statistics().mic_failures == 0);
	QU_CHECK(result.admitted + result.dropped == usb_accepted.size());
	QU_CHECK(usb_accepted.size() + result.usb_rejected == trace.arrivals.size());
	QU_CHECK(peer.messages.size() == result.admitted_indices.size());
	size_t subsequence = 0;
	for (size_t i = 0; i < peer.messages.size(); ++i) {
		size_t index = result.admitted_indices[i];
		while (subsequence < usb_accepted.size() && usb_accepted[subsequence] != index) ++subsequence;
		QU_CHECK(subsequence < usb_accepted.size());
		++subsequence;
		if (i) QU_CHECK(result.admitted_indices[i - 1] < index);
		const auto &expected = trace.arrivals[index];
		const auto &actual = peer.messages[i];
		QU_CHECK(actual.type == 3 && actual.body == std::vector<uint8_t>(expected.body.begin(), expected.body.end()));
		uint32_t latency = actual.at - (start + expected.at);
		QU_CHECK(latency <= deadline);
		result.latencies.push_back(latency);
	}
	auto m = metrics(r).slots[0];
	QU_CHECK(m.serialized == m.retired && m.cleared_fragments == 0);
	QU_CHECK(delta(m.serialized, before.detail(0, 1)) == result.admitted);
	QU_CHECK(r.statistics().queue_full == result.dropped);
	QU_CHECK(result.max_rf_age == std::max(m.max_fifo_age_us, m.max_lane_age_us));
	QU_CHECK(peer.down == 2 + result.admitted);
	auto after = capture(r);
	conservation(before, after);
	QU_CHECK(delta(after.rf(0, 1), before.rf(0, 1)) == result.admitted);
	QU_CHECK(delta(after.rf(0, 2), before.rf(0, 2)) == result.dropped);
	QU_CHECK(delta(after.detail(0, 6), before.detail(0, 6)) == usb_accepted.size());
	QU_CHECK(delta(after.detail(0, 7), before.detail(0, 7)) == usb_accepted.size());
	QU_CHECK(delta(after.detail(0, 8), before.detail(0, 8)) == usb_accepted.size());
	QU_CHECK(delta(after.detail(0, 10), before.detail(0, 10)) == result.dropped);
	QU_CHECK(delta(after.detail(0, 11), before.detail(0, 11)) == 0);
	QU_CHECK(delta(after.extra(0, 1), before.extra(0, 1)) == 0);
	QU_CHECK(delta(after.extra(0, 2), before.extra(0, 2)) == 0);
	QU_CHECK(after.pending(0) == 0);
	// Maxima in FC persist globally; report this replay's measured USB ages
	// and depth, rather than subtracting/resetting a cumulative maximum.
	QU_CHECK(after.extra(0, 0) >= before.extra(0, 0) && after.extra(0, 0) >= result.max_usb_age);
	result.rf_high = m.fifo_high_water;
	result.rf_drop = radio.dropped;
	result.rx_age = radio.max_rx_age;
	if (!result.latencies.empty()) {
		auto sorted = result.latencies;
		std::sort(sorted.begin(), sorted.end());
		result.p50 = sorted[(sorted.size() * 50 + 99) / 100 - 1];
		result.p95 = sorted[(sorted.size() * 95 + 99) / 100 - 1];
		result.maximum = sorted.back();
	}
	return result;
}

void printReplay(const Trace &trace, uint32_t poll_us, const char *policy, const ReplayResult &r)
{
	std::printf("queue replay %-12s poll=%u us %-3s: admitted=%u dropped=%u USB-rejected=%u "
		"maxRFage=%u us maxUSBage=%u us queueHigh(RF/USB)=%u/%u "
		"RF-RX-dropped=%u maxRXage=%u us latency(p50/p95/max)=%u/%u/%u us initial=%u\n",
		trace.name, unsigned(poll_us), policy, r.admitted, r.dropped, r.usb_rejected,
		unsigned(r.max_rf_age), unsigned(r.max_usb_age), r.rf_high, r.usb_high,
		r.rf_drop, unsigned(r.rx_age), unsigned(r.p50), unsigned(r.p95), unsigned(r.maximum), r.initial_admitted);
}

void compareReplay(const Trace &trace, uint32_t poll_us, bool burst)
{
	auto old = replay(trace, poll_us, false);
	auto newer = replay(trace, poll_us, true);
	printReplay(trace, poll_us, "old", old);
	printReplay(trace, poll_us, "new", newer);
	if (burst) QU_CHECK(newer.initial_admitted > old.initial_admitted);
	// Deliberately no assertion that every recorded workload must improve.
	// Rerun the interleaved schedule to check determinism despite global USB
	// counters having accumulated another complete experiment in between.
	auto repeated = replay(trace, poll_us, true);
	QU_CHECK(repeated.admitted_indices == newer.admitted_indices && repeated.latencies == newer.latencies);
	QU_CHECK(repeated.dropped == newer.dropped && repeated.usb_rejected == newer.usb_rejected);
	QU_CHECK(repeated.max_rf_age == newer.max_rf_age && repeated.max_usb_age == newer.max_usb_age);
	QU_CHECK(repeated.rf_high == newer.rf_high && repeated.usb_high == newer.usb_high && repeated.rf_drop == newer.rf_drop);
	QU_CHECK(repeated.initial_admitted == newer.initial_admitted && repeated.rx_age == newer.rx_age);
}

} // namespace

void testInputProfiles()
{
	Fixture f;
	auto &r = f.receiver; Peer peer = ready(r);
	uint32_t generation = r.sessionGeneration(0), version, after_version;
	uint8_t before[64], after[64], diagnostic[] = {0xfd,0};
	QU_CHECK(r.feature(0, diagnostic, sizeof(diagnostic)));
	r.peekFeature(0, before, version);
	QU_CHECK(r.queueInputProfile(0, generation, true));
	r.peekFeature(0, after, after_version);
	QU_CHECK(version == after_version && !std::memcmp(before, after, 64));
	drain(r, peer);
	QU_CHECK(peer.messages.size() == 2);
	QU_CHECK(peer.messages[0].body == std::vector<uint8_t>({0x81,0}));
	QU_CHECK(peer.messages[1].body == std::vector<uint8_t>({0x87,9,49,2,0,7,7,0,8,7,0}));
	peer.messages.clear();
	QU_CHECK(r.queueInputProfile(0, generation, false)); drain(r, peer);
	QU_CHECK(peer.messages.size() == 2 && peer.messages[0].body == std::vector<uint8_t>({0x85,0}) &&
		peer.messages[1].body == std::vector<uint8_t>({0x8e,0}));
	QU_CHECK(!r.queueInputProfile(0, generation + 1, true));
	auto body = pulse(0);
	for (unsigned i = 0; i < 11; ++i) QU_CHECK(r.feature(0, body.data(), unsigned(body.size())));
	QU_CHECK(r.freeFeatureFragments(0) == 1 && !r.queueInputProfile(0, generation, true));
	QU_CHECK(r.freeFeatureFragments(0) == 1); // no half of the profile was enqueued
}

void testRumbleBridge()
{
	context = "Xbox rumble bridge";
	Fixture f; auto &r = f.receiver; Peer peer = ready(r);
	uint32_t generation = r.sessionGeneration(0), version, after_version;
	uint8_t before[64], after[64], diagnostic[] = {0xfd,0};
	QU_CHECK(r.feature(0, diagnostic, sizeof(diagnostic))); r.peekFeature(0, before, version);
	QU_CHECK(r.queueRumble(0, generation, 255, 128));
	QU_CHECK(r.freeFeatureFragments(0) == 10 && !r.queueRumble(0, generation, 1, 1));
	r.peekFeature(0, after, after_version);
	QU_CHECK(version == after_version && !memcmp(before, after, 64));
	r.service(); auto lane0 = r.acknowledgement(2, 0, false), lane1 = r.acknowledgement(2, 1, false);
	r.cancelRumble(0, generation);
	auto after0 = r.acknowledgement(2, 0, false), after1 = r.acknowledgement(2, 1, false);
	QU_CHECK(lane0.size == after0.size && !memcmp(lane0.data, after0.data, lane0.size));
	QU_CHECK(lane1.size == after1.size && !memcmp(lane1.data, after1.data, lane1.size));
	// Drain with exact next CCM counters: cancellation cannot skip serialized lanes.
	drain(r, peer); QU_CHECK(peer.messages.size() == 2);
	QU_CHECK(peer.messages[0].body == std::vector<uint8_t>({0x8f,8,1,0xe8,3,0xa0,15,12,0,0}));
	QU_CHECK(peer.messages[1].body == std::vector<uint8_t>({0x8f,8,0,251,0,0xc9,8,24,0,0}));
	QU_CHECK(metrics(r).slots[0].haptic_admitted == 0); // no invented USB Feature receipts
	peer.messages.clear();
	QU_CHECK(r.queueRumble(0, generation, 1, 1));
	auto host_pulse = pulse(42); QU_CHECK(r.feature(0, host_pulse.data(), unsigned(host_pulse.size())));
	r.cancelRumble(0, generation); QU_CHECK(r.freeFeatureFragments(0) == 11);
	drain(r, peer); QU_CHECK(peer.messages.size() == 1 && peer.messages[0].body ==
		std::vector<uint8_t>(host_pulse.begin(), host_pulse.end()));
	peer.messages.clear();
	QU_CHECK(r.queueRumble(0, generation, 1, 1)); mock_micros += rumble::QUEUE_MAX_AGE_US;
	drain(r, peer); QU_CHECK(peer.messages.empty());
	QU_CHECK(!r.queueRumble(0, generation + 1, 255, 255));

	rumble::Bridge bridge;
	bridge.select(r, 0, generation, 9);
	rumble::Command cmd{0, generation, 9, mock_micros, 200, 0};
	QU_CHECK(bridge.accept(r, cmd)); bridge.service(r, mock_micros); drain(r, peer);
	QU_CHECK(peer.messages.size() == 1 && peer.messages[0].body[2] == 1);
	// Host flood updates do not become a FIFO of vibration or bypass 20 Hz.
	for (unsigned i = 0; i < 100; ++i) { ++mock_micros; cmd.at_us = mock_micros;
		cmd.low = uint8_t(i + 1); QU_CHECK(bridge.accept(r, cmd)); bridge.service(r, mock_micros); }
	QU_CHECK(r.freeFeatureFragments(0) == 12);
	mock_micros += rumble::REFRESH_US; bridge.service(r, mock_micros);
	QU_CHECK(r.freeFeatureFragments(0) == 11);
	cmd.low = 0; cmd.at_us = mock_micros; QU_CHECK(bridge.accept(r, cmd));
	QU_CHECK(r.freeFeatureFragments(0) == 12); // zero cancels unencrypted pulses
	cmd.low = 255; QU_CHECK(bridge.accept(r, cmd));
	mock_micros += rumble::HOST_TIMEOUT_US; bridge.service(r, mock_micros);
	QU_CHECK(r.freeFeatureFragments(0) == 12);
	bridge.select(r, 0, generation, 10); QU_CHECK(!bridge.accept(r, cmd));
	cmd.usb_generation = 10; cmd.at_us = mock_micros; QU_CHECK(bridge.accept(r, cmd));
	bridge.service(r, mock_micros); QU_CHECK(r.freeFeatureFragments(0) == 11);
	bridge.select(r, -1, 0, 0); QU_CHECK(r.freeFeatureFragments(0) == 12);
	bridge.select(r, 0, generation, 10); QU_CHECK(bridge.accept(r, cmd));
	r.disconnect(0); bridge.service(r, mock_micros);
	QU_CHECK(!r.queueRumble(0, generation, 255, 255));
	peer = restore(r); drain(r, peer); peer.messages.clear();
	bridge.service(r, mock_micros); drain(r, peer); QU_CHECK(peer.messages.empty());
	// Refresh and host timeout use modulo microseconds across wrap.
	generation = r.sessionGeneration(0); bridge.select(r, 0, generation, 11);
	mock_micros = 0xfffff000u; cmd = {0, generation, 11, mock_micros, 1, 1};
	QU_CHECK(bridge.accept(r, cmd)); bridge.service(r, mock_micros); drain(r, peer);
	mock_micros += rumble::REFRESH_US; bridge.service(r, mock_micros);
	QU_CHECK(r.freeFeatureFragments(0) == 10); bridge.select(r, -1, 0, 0);
	context = "queue USB";
}

void testXboxIntegration()
{
	context = "Xbox USB/RF integration";
	Fixture f; auto &r = f.receiver;
	usb::configureBoot(input_mode::Mode::Steam, integrationSaveMode, integrationReset);
	QU_CHECK(r.begin(database(), {aes, randomBytes, persist, usb::input, usb::desktop, ::micros, usb::observeInput}));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r, 16);
	uint8_t disconnect[64] = {1,0,3,1,1};
	for (unsigned slot = 0; slot < 4; ++slot) usb::input(slot, disconnect);
	Peer peer = ready(r);
	QU_CHECK(usb::inputMode() == input_mode::Mode::Steam);
	uint8_t enter[] = {0xfd,5,'M','O','D','E',1}, reply[64];
	QU_CHECK(mockFeatureSet(1, enter, sizeof(enter), sizeof(enter)));
	usb::process(r, 16); usb::publish(r);
	QU_CHECK(Adafruit_USBD_HID::instances[1]->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	usb::process(r, 16); QU_CHECK(usb::serviceMode(r));
	mock_micros += 60000; QU_CHECK(!usb::serviceMode(r)); QU_CHECK(integration_resets == 1);
	// Model cold application startup: reload/consume the boot record, recreate
	// protocol state, then restore RF. The old RF session is not retained.
	usb::configureBoot(input_mode::consumeBoot(input_mode::defaults(), integration_boot), integrationSaveMode, integrationReset);
	QU_CHECK(usb::begin());
	QU_CHECK(r.begin(database(), {aes, randomBytes, persist, usb::input, usb::desktop, ::micros, usb::observeInput}));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r, 16); QU_CHECK(usb::serviceMode(r));
	peer = ready(r);
	QU_CHECK(usb::inputMode() == input_mode::Mode::Xbox);
	uint8_t count; const auto *driver = __wrap_usbd_app_driver_get_cb(&count);
	mock_xinput::reset(); driver->reset(0);
	const auto descriptor = TinyUSBDevice.descriptors[0];
	QU_CHECK(driver->open(0, reinterpret_cast<const tusb_desc_interface_t *>(descriptor.data()), 40) == 40);
	auto sendInput = [&](uint32_t buttons) {
		uint8_t body[] = {3,0,uint8_t(buttons),uint8_t(buttons >> 8),uint8_t(buttons >> 16)};
		sc::Packet wire;
		uint8_t header = uint8_t(((peer.up & 7) << 4) | ((peer.down & 1) << 2));
		QU_CHECK(sc::seal(aes, peer.key, peer.iv, peer.up, false, header, body, sizeof(body), wire));
		r.receive(peer.pipe, wire);
		peer.feedback(r.acknowledgement(peer.pipe, unsigned(peer.down & 1), true));
		r.service();
	};
	sendInput(0); QU_CHECK(usb::serviceMode(r)); drain(r, peer); peer.messages.clear();
	sendInput(0x2000); usb::pump();
	QU_CHECK(!mock_xinput::sent.empty() && mock_xinput::sent.back()[2] == 0 && mock_xinput::sent.back()[3] == 4);
	sendInput(0); mock_micros += 4000; mock_xinput::busy[0x81] = false; usb::pump();
	QU_CHECK(mock_xinput::sent.back()[2] == 0 && mock_xinput::sent.back()[3] == 0);
	auto motors = [&](uint8_t low, uint8_t high) {
		uint8_t data[] = {0,8,0,low,high,0,0,0};
		QU_CHECK(mock_xinput::out_buffer != nullptr);
		memcpy(mock_xinput::out_buffer, data, 8); mock_xinput::busy[2] = false;
		QU_CHECK(driver->xfer_cb(0, 2, XFER_RESULT_SUCCESS, 8));
	};
	motors(255,128); QU_CHECK(usb::serviceMode(r)); drain(r, peer);
	QU_CHECK(peer.messages.size() == 2 && peer.messages[0].body[0] == 0x8f && peer.messages[1].body[0] == 0x8f);
	peer.messages.clear(); motors(0,0); QU_CHECK(usb::serviceMode(r));
	mock_micros += rumble::REFRESH_US; QU_CHECK(usb::serviceMode(r)); drain(r, peer); QU_CHECK(peer.messages.empty());
	motors(200,200); QU_CHECK(usb::serviceMode(r)); QU_CHECK(r.freeFeatureFragments(0) == 10);
	mock_micros += 250001; QU_CHECK(usb::serviceMode(r));
	QU_CHECK(r.freeFeatureFragments(0) == 12); drain(r, peer); QU_CHECK(peer.messages.empty());
	// A new authenticated handset session can return the existing Xbox USB
	// personality to Steam without replugging the receiver.
	r.disconnect(0); peer = restore(r); drain(r, peer);
	for (unsigned i = 0; i < 4; ++i) {
		mock_micros += 100000; sendInput(0x2080); QU_CHECK(usb::serviceMode(r));
	}
	mock_micros += 60000; QU_CHECK(!usb::serviceMode(r)); QU_CHECK(integration_resets == 2);
	usb::configureBoot(input_mode::consumeBoot(input_mode::defaults(), integration_boot), integrationSaveMode, integrationReset);
	QU_CHECK(usb::begin());
	QU_CHECK(r.begin(database(), {aes, randomBytes, persist, usb::input, usb::desktop, ::micros, usb::observeInput}));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true); usb::process(r, 16); QU_CHECK(usb::serviceMode(r));
	QU_CHECK(usb::inputMode() == input_mode::Mode::Steam && TinyUSBDevice.pid == 0x1142);
	peer = ready(r); sendInput(0); drain(r, peer);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	context = "queue USB";
}

void testQueueUsb(const char *fixture_path)
{
	assertions = 0;
	testInputProfiles();
	testRumbleBridge();
	testXboxIntegration();
	static_assert(sc::queue_policy::USB_BATCH == 4 && sc::queue_policy::RX_BATCH == 8 &&
		sc::queue_policy::SERVICE_ROUNDS == 4 && sc::queue_policy::HAPTIC_WAIT_US == 8000,
		"update the explicit policy comparison when queue policy changes");
	testWait(true, false);
	testWait(false, false);
	testWait(true, true);
	testWait(false, true);
	testCancellationAndOffline();
	testBestEffortAndSettings();
	testFcCodec();
	auto traces = readTraces(fixture_path);
	Trace burst{"burst-16", {}};
	for (unsigned i = 0; i < 16; ++i) burst.arrivals.push_back({0, pulse(i)});
	std::puts("Queue policy replay: deterministic software schedules, safe authenticated phase gates for both; "
		"maxRFage=max(FIFO,lane), latency=peer receipt minus USB arrival; no physical timing conclusions.");
	for (uint32_t interval : {4000u, 8000u}) {
		compareReplay(burst, interval, true);
		for (const auto &trace : traces) compareReplay(trace, interval, false);
	}
	// An explicit ring-overflow schedule exercises rejected host writes even
	// if neither captured workload happens to fill the 31-record USB ring.
	Trace overflow{"USB-ring-32", {}};
	for (unsigned i = 0; i < 32; ++i) overflow.arrivals.push_back({0, pulse(i)});
	for (bool newer : {false, true}) {
		auto result = replay(overflow, 4000, newer);
		QU_CHECK(result.usb_rejected == 1 && result.usb_high == 31);
		printReplay(overflow, 4000, newer ? "new" : "old", result);
	}
	// Fence callbacks from the final, now-expired stack Receiver. The next
	// fixture installs its own source with reset/process/publish.
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	std::printf("PASS: queue USB deadlines, wrap, cancellations, FC snapshots and authenticated arrival replay (%u assertions)\n", assertions);
}

#undef QU_CHECK
