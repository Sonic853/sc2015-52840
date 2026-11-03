#include "protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace {

unsigned assertions;
#define QUEUE_CHECK(value) do { ++assertions; if (!(value)) { \
	std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

// Local versions of the existing host fixtures: actual Receiver/CCM and host
// AES, with a controllable clock and receiver-only serialization failure.
bool aesBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
#ifdef _WIN32
	BCRYPT_ALG_HANDLE provider = nullptr;
	BCRYPT_KEY_HANDLE handle = nullptr;
	QUEUE_CHECK(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0);
	QUEUE_CHECK(BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
		ULONG(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0) == 0);
	QUEUE_CHECK(BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0,
		const_cast<PUCHAR>(key), 16, 0) == 0);
	ULONG count = 0;
	bool ok = BCryptEncrypt(handle, const_cast<PUCHAR>(in), 16, nullptr,
		nullptr, 0, out, 16, &count, 0) == 0 && count == 16;
	BCryptDestroyKey(handle);
	BCryptCloseAlgorithmProvider(provider, 0);
	return ok;
#else
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	QUEUE_CHECK(ctx);
	int count = 0;
	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, out, &count, in, 16) == 1 && count == 16;
	EVP_CIPHER_CTX_free(ctx);
	return ok;
#endif
}

uint32_t now;
unsigned random_byte, saves;
bool fail_aes;

bool receiverAes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	return !fail_aes && aesBlock(key, in, out);
}

uint32_t clockUs() { return now; }

bool randomBytes(uint8_t *data, size_t size)
{
	for (size_t i = 0; i < size; ++i)
		data[i] = uint8_t(++random_byte);
	return true;
}

bool persist(const sc::Database &) { ++saves; return true; }
void input(unsigned, const uint8_t[64]) {}

// Omitting the trailing clock must remain valid for all existing aggregates.
const sc::Hooks legacy_hooks = {receiverAes, randomBytes, persist, input, nullptr};
const sc::Hooks timed_hooks = {receiverAes, randomBytes, persist, input, nullptr, clockUs};

sc::Database database()
{
	sc::Database db{};
	const uint8_t address[] = {0x28, 0x36, 0x57, 0x62, 0x94};
	std::memcpy(db.address, address, sizeof(address));
	return db;
}

struct Fixture {
	sc::Receiver receiver;
	explicit Fixture(bool timed = true, uint32_t initial_time = 0)
	{
		now = initial_time;
		random_byte = saves = 0;
		fail_aes = false;
		QUEUE_CHECK(receiver.begin(database(), timed ? timed_hooks : legacy_hooks));
		QUEUE_CHECK(receiver.queueClockAvailable() == timed);
	}
};

sc::QueueMetrics snapshot(const sc::Receiver &receiver)
{
	sc::QueueMetrics result;
	receiver.copyQueueMetrics(result);
	return result;
}

void requireMetrics(const sc::QueueSlotMetrics &actual, const sc::QueueSlotMetrics &expected)
{
	QUEUE_CHECK(actual.admitted == expected.admitted);
	QUEUE_CHECK(actual.haptic_admitted == expected.haptic_admitted);
	QUEUE_CHECK(actual.haptic_full == expected.haptic_full);
	for (unsigned i = 0; i < 5; ++i)
		QUEUE_CHECK(actual.a2_a6_full[i] == expected.a2_a6_full[i]);
	QUEUE_CHECK(actual.other_full == expected.other_full);
	QUEUE_CHECK(actual.initial_retries == expected.initial_retries);
	QUEUE_CHECK(actual.count_retries == expected.count_retries);
	QUEUE_CHECK(actual.commit_waits == expected.commit_waits);
	QUEUE_CHECK(actual.offline == expected.offline);
	QUEUE_CHECK(actual.serialized == expected.serialized);
	QUEUE_CHECK(actual.retired == expected.retired);
	QUEUE_CHECK(actual.cleared_fragments == expected.cleared_fragments);
	QUEUE_CHECK(actual.max_fifo_age_us == expected.max_fifo_age_us);
	QUEUE_CHECK(actual.max_lane_age_us == expected.max_lane_age_us);
	QUEUE_CHECK(actual.fifo_high_water == expected.fifo_high_water);
}

const sc::QueueEvent &eventAt(const sc::QueueMetrics &metrics, unsigned chronological_index)
{
	QUEUE_CHECK(chronological_index < metrics.event_count);
	return metrics.events[(metrics.event_head + 16 - metrics.event_count + chronological_index) % 16];
}

struct Peer {
	uint8_t key[16]{}, iv[8]{};
	unsigned slot = 0;
	uint64_t up = 0, down = 0;

	void send(sc::Receiver &receiver, unsigned type, const uint8_t *body, unsigned size,
		  bool advance = true)
	{
		if (advance)
			++up;
		uint8_t header = uint8_t(type | ((up & 7) << 4) | ((down & 1) << 2));
		sc::Packet wire{};
		QUEUE_CHECK(sc::seal(aesBlock, key, iv, up, false, header, body, size, wire));
		receiver.receive(slot + 2, wire);
	}

	std::vector<uint8_t> read(sc::Receiver &receiver, unsigned type)
	{
		const uint8_t hello[] = {4};
		send(receiver, 1, hello, sizeof(hello));
		sc::Packet wire = receiver.acknowledgement(slot + 2, unsigned(down & 1));
		QUEUE_CHECK(wire.size > 1 && unsigned(wire.data[0] & 3) == type);
		uint8_t body[sc::MAX_PLAIN];
		unsigned size = 0;
		QUEUE_CHECK(sc::open(aesBlock, key, iv, down + 1, true, wire, body, size));
		++down;
		send(receiver, 1, hello, sizeof(hello));
		receiver.service();
		return std::vector<uint8_t>(body, body + size);
	}

	void rekey(sc::Receiver &receiver)
	{
		uint8_t update[17] = {1};
		for (unsigned i = 1; i < sizeof(update); ++i)
			update[i] = uint8_t(0xd0 + i);
		send(receiver, 1, update, sizeof(update));
		std::memcpy(key, update + 1, 16);
		down = 0;
	}
};

Peer joinPeer(sc::Receiver &receiver, unsigned serial = 0)
{
	Peer peer;
	receiver.pairing(true);
	sc::Packet advertisement = receiver.acknowledgement(1, 0);
	QUEUE_CHECK(advertisement.size == 22);
	peer.slot = (advertisement.data[9] & 7) - 2;
	QUEUE_CHECK(peer.slot < sc::SLOT_COUNT);
	std::memcpy(peer.key, sc::BOOTSTRAP_KEY, 16);
	std::memcpy(peer.iv, advertisement.data + 1, 4);
	uint8_t body[17];
	std::memcpy(body, receiver.database().address, 5);
	for (unsigned i = 0; i < 8; ++i)
		body[i + 5] = uint8_t(0x30 + serial * 8 + i);
	for (unsigned i = 0; i < 4; ++i)
		body[i + 13] = peer.iv[i + 4] = uint8_t(0xa0 + serial + i);
	sc::Packet wire{};
	QUEUE_CHECK(sc::seal(aesBlock, peer.key, advertisement.data + 1, 0, false,
			     1, body, sizeof(body), wire));
	receiver.receive(1, wire);
	QUEUE_CHECK(receiver.connected(peer.slot) && receiver.pending(peer.slot));
	return peer;
}

void requireEmpty(const sc::Receiver &receiver, const Peer &peer)
{
	QUEUE_CHECK(receiver.acknowledgement(peer.slot + 2, unsigned(peer.down & 1)).size == 1);
	QUEUE_CHECK(receiver.statistics().mic_failures == 0 && receiver.statistics().invalid == 0);
}

void drainInitial(sc::Receiver &receiver, Peer &peer)
{
	receiver.service();
	QUEUE_CHECK(peer.read(receiver, 1) == std::vector<uint8_t>({4, 0, 0, 0, 0x55, 0xf7, 0x16, 0x0e}));
	QUEUE_CHECK(peer.read(receiver, 1) == std::vector<uint8_t>({3, 1}));
	requireEmpty(receiver, peer);
}

const uint8_t pulse[] = {0x8f, 7, 0, 0x2c, 1, 0x90, 1, 2, 0};

void pulses(sc::Receiver &receiver, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		QUEUE_CHECK(receiver.feature(0, pulse, sizeof(pulse)));
}

void readPulses(sc::Receiver &receiver, Peer &peer, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		QUEUE_CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(pulse, pulse + sizeof(pulse)));
}

void testAdmissionAndFullClasses()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	uint8_t padded[64] = {0x87, 0}, large[64] = {0x87, 62};
	std::memset(padded + 2, 0x5a, 62);
	for (unsigned i = 2; i < sizeof(large); ++i)
		large[i] = uint8_t(i);
	for (unsigned i = 0; i < 10; ++i)
		QUEUE_CHECK(receiver.feature(0, padded, sizeof(padded)));
	// Two free entries cannot admit any part of a three-fragment command.
	QUEUE_CHECK(!receiver.canQueueFeature(0, large, sizeof(large)));
	QUEUE_CHECK(!receiver.feature(0, large, sizeof(large)));
	pulses(receiver, 2);
	const uint8_t blocked[] = {0x8f, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0x87, 0x83};
	for (uint8_t command : blocked) {
		const uint8_t request[] = {command, 0};
		QUEUE_CHECK(!receiver.feature(0, request, sizeof(request)));
	}
	for (unsigned i = 0; i < 20; ++i)
		QUEUE_CHECK(!receiver.canQueueFeature(0, large, sizeof(large)));
	sc::QueueSlotMetrics expected{};
	expected.admitted = 12;
	expected.haptic_admitted = 2;
	expected.haptic_full = 1;
	for (auto &count : expected.a2_a6_full) count = 1;
	expected.other_full = 3;
	expected.serialized = expected.retired = 2;
	expected.fifo_high_water = 12;
	auto metrics = snapshot(receiver);
	requireMetrics(metrics.slots[0], expected);
	for (unsigned i = 1; i < sc::SLOT_COUNT; ++i) requireMetrics(metrics.slots[i], {});
	QUEUE_CHECK(receiver.statistics().queue_full == 9 && metrics.event_count == 9);
	const auto &first = eventAt(metrics, 0);
	QUEUE_CHECK(first.id == 1 && first.command == 0x87 && first.length == 64 && first.needed == 3);
	QUEUE_CHECK(first.kind == uint8_t(sc::QueueEventKind::OtherFull) && first.fifo_count == 10);
	QUEUE_CHECK(first.generation == receiver.sessionGeneration(0) && first.slot == 0);
	QUEUE_CHECK((first.lane_flags & 3) == 0 && first.usb_depth == 0 && first.age_us == 0);
	for (unsigned i = 0; i < sizeof(blocked); ++i) {
		const auto &event = eventAt(metrics, i + 1);
		const auto kind = i == 0 ? sc::QueueEventKind::HapticFull :
			i <= 5 ? sc::QueueEventKind::BestEffortFull : sc::QueueEventKind::OtherFull;
		QUEUE_CHECK(event.id == i + 2 && event.command == blocked[i] && event.kind == uint8_t(kind));
		QUEUE_CHECK(event.length == 2 && event.needed == 1 && event.fifo_count == 12);
	}
	// FC remains usable with a full FIFO; only USB supplies its wire pages.
	static_assert(sc::localFeature(0xfc) && !sc::localFeature(0xfd), "FC extension boundary");
	const uint8_t diagnostic[] = {0xfc, 0};
	QUEUE_CHECK(receiver.canQueueFeature(0, diagnostic, sizeof(diagnostic)));
	QUEUE_CHECK(receiver.feature(0, diagnostic, sizeof(diagnostic)));
	uint8_t reply[64];
	uint32_t version;
	receiver.peekFeature(0, reply, version);
	QUEUE_CHECK(reply[0] == 0xfc && reply[1] == 62 && reply[2] == 1);
	for (unsigned i = 3; i < sizeof(reply); ++i) QUEUE_CHECK(reply[i] == 0);
	requireMetrics(snapshot(receiver).slots[0], expected);
	receiver.service();
	for (unsigned i = 0; i < 10; ++i)
		QUEUE_CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0x87, 0}));
	readPulses(receiver, peer, 2);
	requireEmpty(receiver, peer);
	QUEUE_CHECK(receiver.feature(0, large, sizeof(large)));
	receiver.service();
	std::vector<uint8_t> actual;
	for (unsigned i = 0; i < 3; ++i) {
		auto part = peer.read(receiver, i == 2 ? 3 : 2);
		actual.insert(actual.end(), part.begin(), part.end());
	}
	QUEUE_CHECK(actual == std::vector<uint8_t>(large, large + sizeof(large)));
	++expected.admitted; // Three fragments, one admitted command.
	expected.serialized = expected.retired = 17;
	requireMetrics(snapshot(receiver).slots[0], expected);
	requireEmpty(receiver, peer);
	large[0] = 0x8f;
	QUEUE_CHECK(receiver.feature(0, large, sizeof(large)));
	receiver.service();
	actual.clear();
	for (unsigned i = 0; i < 3; ++i) {
		auto part = peer.read(receiver, i == 2 ? 3 : 2);
		actual.insert(actual.end(), part.begin(), part.end());
	}
	QUEUE_CHECK(actual == std::vector<uint8_t>(large, large + sizeof(large)));
	++expected.admitted;
	++expected.haptic_admitted;
	expected.serialized = expected.retired = 20;
	requireMetrics(snapshot(receiver).slots[0], expected);
	requireEmpty(receiver, peer);
}

void testQueriesAndOffline()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	const uint8_t offline_commands[] = {0x87, 0x8f, 0x83, 0xa2};
	for (uint8_t command : offline_commands) {
		const uint8_t request[] = {command, 0};
		QUEUE_CHECK(receiver.feature(2, request, sizeof(request)));
	}
	const uint8_t diagnostic[] = {0xfc, 0}, query[] = {0x83, 0};
	QUEUE_CHECK(receiver.feature(2, diagnostic, sizeof(diagnostic)));
	QUEUE_CHECK(!receiver.feature(2, nullptr, 2));
	QUEUE_CHECK(!receiver.feature(2, query, 1));
	QUEUE_CHECK(!receiver.feature(sc::SLOT_COUNT, query, sizeof(query)));
	QUEUE_CHECK(!receiver.featureForSession(2, 0, query, sizeof(query)));
	sc::QueueSlotMetrics offline{};
	offline.offline = 4;
	auto metrics = snapshot(receiver);
	requireMetrics(metrics.slots[2], offline);
	QUEUE_CHECK(metrics.event_count == 4 && receiver.statistics().queue_full == 0);
	for (unsigned i = 0; i < metrics.event_count; ++i) {
		const auto &event = eventAt(metrics, i);
		QUEUE_CHECK(event.kind == uint8_t(sc::QueueEventKind::Offline) && event.slot == 2);
		QUEUE_CHECK(event.command == offline_commands[i] && event.length == 2 && event.needed == 1);
		QUEUE_CHECK(event.generation == 1 && event.fifo_count == 0 && event.lane_flags == 0);
	}
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	QUEUE_CHECK(receiver.feature(0, query, sizeof(query)));
	for (unsigned i = 0; i < 10; ++i) QUEUE_CHECK(receiver.feature(0, query, sizeof(query)));
	receiver.service();
	QUEUE_CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0x83, 0}));
	const uint8_t response[] = {0x83, 1, 0x42};
	peer.send(receiver, 3, response, sizeof(response));
	for (unsigned i = 0; i < 10; ++i) QUEUE_CHECK(receiver.feature(0, query, sizeof(query)));
	sc::QueueSlotMetrics expected{};
	expected.admitted = 1;
	expected.serialized = expected.retired = 3;
	expected.fifo_high_water = 2;
	requireMetrics(snapshot(receiver).slots[0], expected);
	uint8_t reply[64];
	uint32_t version;
	receiver.peekFeature(0, reply, version);
	receiver.consumeFeature(0, version);
	QUEUE_CHECK(receiver.feature(0, query, sizeof(query)));
	receiver.service();
	QUEUE_CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0x83, 0}));
	const uint8_t forwarded[] = {0xfd, 0};
	QUEUE_CHECK(receiver.feature(0, forwarded, sizeof(forwarded)));
	receiver.service();
	QUEUE_CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0xfd, 0}));
	expected.admitted = 3;
	expected.serialized = expected.retired = 5;
	requireMetrics(snapshot(receiver).slots[0], expected);
	requireMetrics(snapshot(receiver).slots[2], offline);
	QUEUE_CHECK(snapshot(receiver).event_count == 4 && receiver.statistics().queue_full == 0);
	requireEmpty(receiver, peer);
}

void testInternalRetries()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	// Fill before the first control04 is admitted. It stays pending until
	// space returns; repeated service attempts are not lost Feature commands.
	pulses(receiver, 12);
	receiver.service();
	pulses(receiver, 2); // Occupy both lanes plus all twelve FIFO entries.
	receiver.service();
	receiver.service();
	sc::QueueSlotMetrics expected{};
	expected.admitted = expected.haptic_admitted = 14;
	expected.initial_retries = 3;
	expected.serialized = 2;
	expected.fifo_high_water = 12;
	requireMetrics(snapshot(receiver).slots[0], expected);
	QUEUE_CHECK(receiver.statistics().queue_full == 3);
	readPulses(receiver, peer, 14);
	QUEUE_CHECK(peer.read(receiver, 1)[0] == 4);
	QUEUE_CHECK(peer.read(receiver, 1) == std::vector<uint8_t>({3, 1}));
	expected.initial_retries = 4;
	expected.count_retries = 1;
	expected.serialized = expected.retired = 16;
	auto metrics = snapshot(receiver);
	requireMetrics(metrics.slots[0], expected);
	QUEUE_CHECK(receiver.statistics().queue_full == 5 && metrics.event_count == 5);
	for (unsigned i = 0; i < 5; ++i) {
		const auto &event = eventAt(metrics, i);
		QUEUE_CHECK(event.kind == uint8_t(i < 4 ? sc::QueueEventKind::InitialRetry : sc::QueueEventKind::CountRetry));
		QUEUE_CHECK(event.command == (i < 4 ? 4 : 3) && event.length == (i < 4 ? 8 : 2));
		QUEUE_CHECK(event.needed == 1 && event.fifo_count == 12);
	}
	requireEmpty(receiver, peer);
}

void testSecondPeerCountRetries()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	pulses(receiver, 12);
	receiver.service();
	pulses(receiver, 2);
	Peer second = joinPeer(receiver, 1);
	QUEUE_CHECK(second.slot == 1);
	receiver.service();
	receiver.service();
	sc::QueueSlotMetrics expected{};
	expected.admitted = expected.haptic_admitted = 14;
	expected.count_retries = 2;
	expected.serialized = 4;
	expected.retired = 2;
	expected.fifo_high_water = 12;
	requireMetrics(snapshot(receiver).slots[0], expected);
	readPulses(receiver, peer, 14);
	QUEUE_CHECK(peer.read(receiver, 1) == std::vector<uint8_t>({3, 2}));
	expected.count_retries = 3;
	expected.serialized = expected.retired = 17;
	auto metrics = snapshot(receiver);
	requireMetrics(metrics.slots[0], expected);
	sc::QueueSlotMetrics other{};
	other.serialized = other.fifo_high_water = 2;
	requireMetrics(metrics.slots[1], other);
	QUEUE_CHECK(receiver.statistics().queue_full == 3 && metrics.event_count == 3);
	for (unsigned i = 0; i < 3; ++i) {
		const auto &event = eventAt(metrics, i);
		QUEUE_CHECK(event.kind == uint8_t(sc::QueueEventKind::CountRetry) && event.slot == 0);
	}
	requireEmpty(receiver, peer);
}

void testCommitWaits()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	peer.rekey(receiver);
	drainInitial(receiver, peer); // New-key proof makes the bond committable.
	pulses(receiver, 12);
	receiver.service();
	pulses(receiver, 2);
	const uint8_t commit[] = {0xb3, 0};
	QUEUE_CHECK(!receiver.feature(0, commit, sizeof(commit)));
	for (unsigned i = 0; i < 3; ++i) receiver.service();
	sc::QueueSlotMetrics expected{};
	expected.admitted = expected.haptic_admitted = 14;
	expected.commit_waits = 4;
	expected.serialized = 4;
	expected.retired = 2;
	expected.fifo_high_water = 12;
	requireMetrics(snapshot(receiver).slots[0], expected);
	QUEUE_CHECK(receiver.pending(0) && saves == 0 && receiver.statistics().queue_full == 0);
	readPulses(receiver, peer, 14);
	QUEUE_CHECK(peer.read(receiver, 1) == std::vector<uint8_t>({5}));
	expected.commit_waits = 5; // First retirement frees a lane, not FIFO capacity.
	expected.serialized = expected.retired = 17;
	requireMetrics(snapshot(receiver).slots[0], expected);
	QUEUE_CHECK(!receiver.pending(0) && saves == 1 && receiver.statistics().queue_full == 0);
	QUEUE_CHECK(snapshot(receiver).event_count == 0);
	requireEmpty(receiver, peer);
}

void testAgesAndSessionClears()
{
	Fixture fixture(true, 100);
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	uint8_t large[64] = {0x87, 62};
	now = 120;
	QUEUE_CHECK(receiver.feature(0, large, sizeof(large)));
	now = 150;
	fail_aes = true;
	receiver.service();
	sc::QueueSlotMetrics expected{};
	expected.admitted = 1;
	expected.serialized = expected.retired = 2;
	expected.fifo_high_water = 3;
	expected.max_fifo_age_us = 30;
	requireMetrics(snapshot(receiver).slots[0], expected);
	now = 170;
	fail_aes = false;
	receiver.service();
	now = 190;
	expected.serialized = 4;
	expected.max_fifo_age_us = 70; // Pending third fragment, not just the last serialization.
	expected.max_lane_age_us = 20;
	requireMetrics(snapshot(receiver).slots[0], expected);
	// USB can be preempted after capturing its cutoff. The protocol owner has
	// not changed, so a later copy must retain ages at that exact cutoff.
	now = 195;
	sc::QueueMetrics frozen;
	receiver.copyQueueMetrics(frozen, 190);
	requireMetrics(frozen.slots[0], expected);
	now = 210;
	QUEUE_CHECK(peer.read(receiver, 2).size() == 27);
	now = 225;
	QUEUE_CHECK(peer.read(receiver, 2).size() == 27);
	now = 230;
	QUEUE_CHECK(peer.read(receiver, 3).size() == 10);
	expected.serialized = expected.retired = 5;
	expected.max_fifo_age_us = 90;
	expected.max_lane_age_us = 55;
	requireMetrics(snapshot(receiver).slots[0], expected);
	// Invalid lanes and duplicate feedback cannot be retired a second time.
	const uint8_t hello[] = {4};
	peer.send(receiver, 1, hello, sizeof(hello), false);
	receiver.service();
	requireMetrics(snapshot(receiver).slots[0], expected);
	const uint8_t short_write[] = {0x87, 0};
	now = 240;
	QUEUE_CHECK(receiver.feature(0, short_write, sizeof(short_write)));
	now = 250;
	receiver.service();
	now = 260;
	QUEUE_CHECK(receiver.feature(0, large, sizeof(large)));
	receiver.service(); // Two valid lanes and two queued fragments remain.
	now = 400;
	receiver.disconnect(0);
	expected.admitted = 3;
	expected.serialized = 7;
	expected.cleared_fragments = 4;
	expected.max_fifo_age_us = 140;
	expected.max_lane_age_us = 150;
	now = 1000;
	requireMetrics(snapshot(receiver).slots[0], expected);
	peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	expected.serialized = 9;
	expected.retired = 7;
	requireMetrics(snapshot(receiver).slots[0], expected);
	receiver.disconnect(0); // Empty reset must not count normal retirements again.
	requireMetrics(snapshot(receiver).slots[0], expected);
	QUEUE_CHECK(receiver.begin(database(), timed_hooks));
	auto reset = snapshot(receiver);
	for (const auto &slot : reset.slots) requireMetrics(slot, {});
	QUEUE_CHECK(reset.event_count == 0 && reset.event_head == 0 && reset.next_event_id == 1);
}

void testTransportClearAndBootstrap()
{
	Fixture fixture(true, 10);
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	receiver.service(); // Two old-key controls have been serialized, none retired.
	now = 20;
	pulses(receiver, 1);
	now = 50;
	peer.rekey(receiver);
	sc::QueueSlotMetrics expected{};
	expected.admitted = expected.haptic_admitted = 1;
	expected.serialized = 2;
	expected.cleared_fragments = 3;
	expected.max_fifo_age_us = 30;
	expected.max_lane_age_us = 40;
	expected.fifo_high_water = 2;
	requireMetrics(snapshot(receiver).slots[0], expected);
	// The independently cached bootstrap control04 is not a queue fragment.
	QUEUE_CHECK(receiver.acknowledgement(2, 0).size == 13);
	receiver.service();
	expected.serialized = 4;
	now = 80;
	receiver.disconnect(0);
	expected.cleared_fragments = 5;
	requireMetrics(snapshot(receiver).slots[0], expected);
	QUEUE_CHECK(receiver.statistics().queue_full == 0);
}

void testClockWrapAndUnavailable()
{
	{
		Fixture fixture(true, 0xfffffff0u);
		auto &receiver = fixture.receiver;
		Peer peer = joinPeer(receiver);
		drainInitial(receiver, peer);
		uint8_t large[64] = {0x87, 62};
		QUEUE_CHECK(receiver.feature(0, large, sizeof(large)));
		now = 0x10;
		receiver.service();
		now = 0x30;
		auto metrics = snapshot(receiver);
		QUEUE_CHECK(metrics.slots[0].max_fifo_age_us == 64 && metrics.slots[0].max_lane_age_us == 32);
		now = 0x40;
		QUEUE_CHECK(peer.read(receiver, 2).size() == 27);
		now = 0x50;
		QUEUE_CHECK(peer.read(receiver, 2).size() == 27);
		now = 0x60;
		QUEUE_CHECK(peer.read(receiver, 3).size() == 10);
		sc::QueueSlotMetrics expected{};
		expected.admitted = 1;
		expected.serialized = expected.retired = 5;
		expected.fifo_high_water = 3;
		expected.max_fifo_age_us = 80;
		expected.max_lane_age_us = 64;
		requireMetrics(snapshot(receiver).slots[0], expected);
		receiver.disconnect(0);
		requireMetrics(snapshot(receiver).slots[0], expected);
	}
	{
		Fixture fixture(false, 100);
		auto &receiver = fixture.receiver;
		Peer peer = joinPeer(receiver);
		drainInitial(receiver, peer);
		pulses(receiver, 12);
		sc::QueueMetrics unavailable;
		receiver.copyQueueMetrics(unavailable, 1234);
		QUEUE_CHECK(unavailable.slots[0].max_fifo_age_us == 0);
		now = 0xfffffff0u;
		QUEUE_CHECK(!receiver.feature(0, pulse, sizeof(pulse)));
		receiver.service();
		now = 50;
		readPulses(receiver, peer, 1);
		receiver.disconnect(0);
		sc::QueueSlotMetrics expected{};
		expected.admitted = expected.haptic_admitted = 12;
		expected.haptic_full = 1;
		expected.serialized = 5;
		expected.retired = 3;
		expected.cleared_fragments = 11;
		expected.fifo_high_water = 12;
		auto metrics = snapshot(receiver);
		requireMetrics(metrics.slots[0], expected);
		QUEUE_CHECK(metrics.event_count == 1 && eventAt(metrics, 0).at_us == 0);
	}
}

void testEventRing()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	const sc::QueueEventKind kinds[] = {
		sc::QueueEventKind::UsbWait, sc::QueueEventKind::UsbTimeout, sc::QueueEventKind::StaleUsb,
	};
	for (unsigned i = 0; i < 20; ++i) {
		now = 100 + i;
		receiver.queueEvent(i % 4, kinds[i % 3], 0x87, 64, 3, i * 7, uint8_t(i % 8));
	}
	receiver.queueEvent(sc::SLOT_COUNT, sc::QueueEventKind::Offline, 0x87, 2, 1);
	auto metrics = snapshot(receiver);
	QUEUE_CHECK(metrics.event_head == 4 && metrics.event_count == 16 && metrics.next_event_id == 21);
	for (unsigned n = 0; n < 16; ++n) {
		unsigned i = n + 4;
		const auto &event = eventAt(metrics, n);
		QUEUE_CHECK(event.id == i + 1 && event.at_us == 100 + i && event.generation == 1);
		QUEUE_CHECK(event.age_us == i * 7 && event.slot == i % 4 && event.kind == uint8_t(kinds[i % 3]));
		QUEUE_CHECK(event.command == 0x87 && event.length == 64 && event.needed == 3);
		QUEUE_CHECK(event.fifo_count == 0 && event.lane_flags == 0 && event.usb_depth == i % 8);
	}
	for (const auto &slot : metrics.slots) requireMetrics(slot, {});
	Peer peer = joinPeer(receiver);
	receiver.service();
	const uint8_t hello[] = {4};
	peer.send(receiver, 1, hello, sizeof(hello));
	pulses(receiver, 3);
	now = 200;
	receiver.queueEvent(0, sc::QueueEventKind::UsbWait, 0x87, 64, 3, 81, 7);
	metrics = snapshot(receiver);
	const auto live = eventAt(metrics, 15);
	QUEUE_CHECK(live.id == 21 && live.generation == receiver.sessionGeneration(0));
	QUEUE_CHECK(live.fifo_count == 3 && live.lane_flags == 0x0b && live.age_us == 81 && live.usb_depth == 7);
	receiver.disconnect(0);
	receiver.queueEvent(0, sc::QueueEventKind::StaleUsb, 0x87, 64, 3);
	metrics = snapshot(receiver);
	const auto &stale = eventAt(metrics, 15);
	QUEUE_CHECK(stale.id == 22 && stale.generation == receiver.sessionGeneration(0));
	QUEUE_CHECK(stale.generation != live.generation && stale.fifo_count == 0 && stale.lane_flags == 0);
	QUEUE_CHECK(eventAt(metrics, 14).generation == live.generation);
	QUEUE_CHECK(metrics.slots[0].cleared_fragments == 5 && metrics.slots[0].retired == 0);
	// A caller can change its copy without corrupting the live record ring.
	metrics.next_event_id = 0;
	metrics.events[0].id = 0;
	QUEUE_CHECK(snapshot(receiver).next_event_id == 23);
	QUEUE_CHECK(eventAt(snapshot(receiver), 0).id == 7);
	QUEUE_CHECK(receiver.begin(database(), timed_hooks));
	metrics = snapshot(receiver);
	QUEUE_CHECK(metrics.event_count == 0 && metrics.event_head == 0 && metrics.next_event_id == 1);
	for (const auto &event : metrics.events) QUEUE_CHECK(event.id == 0);
	for (const auto &slot : metrics.slots) requireMetrics(slot, {});
}

} // namespace

void testQueueMetrics()
{
	assertions = 0;
	testAdmissionAndFullClasses();
	testQueriesAndOffline();
	testInternalRetries();
	testSecondPeerCountRetries();
	testCommitWaits();
	testAgesAndSessionClears();
	testTransportClearAndBootstrap();
	testClockWrapAndUnavailable();
	testEventRing();
	std::printf("PASS: queue metrics, whole-command admission, retries, clocks, clears and bounded event ring (%u assertions)\n", assertions);
}

#undef QUEUE_CHECK
