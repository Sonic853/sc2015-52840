#include "protocol.h"
#include "radio_policy.h"
#include <array>
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

unsigned requirements;
#define REQUIRE(value) do { ++requirements; if (!(value)) { \
	std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

bool aesBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
#ifdef _WIN32
	BCRYPT_ALG_HANDLE provider = nullptr;
	BCRYPT_KEY_HANDLE handle = nullptr;
	REQUIRE(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0);
	REQUIRE(BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
		ULONG(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0) == 0);
	REQUIRE(BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0,
		const_cast<PUCHAR>(key), 16, 0) == 0);
	ULONG count = 0;
	bool ok = BCryptEncrypt(handle, const_cast<PUCHAR>(in), 16, nullptr,
		nullptr, 0, out, 16, &count, 0) == 0 && count == 16;
	BCryptDestroyKey(handle);
	BCryptCloseAlgorithmProvider(provider, 0);
	return ok;
#else
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	REQUIRE(ctx);
	int count = 0;
	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, out, &count, in, 16) == 1 && count == 16;
	EVP_CIPHER_CTX_free(ctx);
	return ok;
#endif
}

struct AesFault {
	unsigned calls = 0, fail_call = 0, failures = 0;
} aes_fault;

bool receiverAes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	if (++aes_fault.calls == aes_fault.fail_call) {
		++aes_fault.failures;
		return false;
	}
	return aesBlock(key, in, out);
}

struct Report {
	unsigned slot;
	std::array<uint8_t, 64> bytes;
};

struct Capture {
	unsigned random_byte = 0, saves = 0, desktop_calls = 0;
	sc::Database stored{};
	std::vector<Report> reports;
	std::array<uint8_t, 8> keyboard{};
	std::array<uint8_t, 4> mouse{};
} capture;

bool randomBytes(uint8_t *data, size_t size)
{
	for (size_t i = 0; i < size; ++i)
		data[i] = uint8_t(++capture.random_byte);
	return true;
}

bool persist(const sc::Database &db)
{
	++capture.saves;
	capture.stored = db;
	return true;
}

void input(unsigned slot, const uint8_t bytes[64])
{
	Report report{};
	report.slot = slot;
	std::memcpy(report.bytes.data(), bytes, 64);
	capture.reports.push_back(report);
}

void desktop(const uint8_t keyboard[8], const uint8_t mouse[4])
{
	++capture.desktop_calls;
	std::memcpy(capture.keyboard.data(), keyboard, 8);
	std::memcpy(capture.mouse.data(), mouse, 4);
}

const sc::Hooks hooks = {receiverAes, randomBytes, persist, input, desktop};

sc::Database database()
{
	sc::Database db{};
	const uint8_t address[5] = {0x28, 0x36, 0x57, 0x62, 0x94};
	std::memcpy(db.address, address, sizeof(address));
	return db;
}

struct Fixture {
	sc::Receiver receiver;
	explicit Fixture(const sc::Database &db = database())
	{
		capture = Capture{};
		aes_fault = AesFault{};
		REQUIRE(receiver.begin(db, hooks));
	}
};

uint32_t get32(const uint8_t *data)
{
	return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
	       (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

void requireZero(const uint8_t *data, unsigned size)
{
	for (unsigned i = 0; i < size; ++i)
		REQUIRE(data[i] == 0);
}

void requireDatabase(const sc::Database &actual, const sc::Database &expected)
{
	REQUIRE(!std::memcmp(actual.address, expected.address, 5));
	for (unsigned i = 0; i < sc::BOND_COUNT; ++i) {
		REQUIRE(actual.bonds[i].used == expected.bonds[i].used);
		REQUIRE(!std::memcmp(actual.bonds[i].id, expected.bonds[i].id, 8));
		REQUIRE(!std::memcmp(actual.bonds[i].key, expected.bonds[i].key, 16));
	}
}

std::array<uint8_t, 64> peek(const sc::Receiver &receiver, unsigned slot,
			   uint32_t *version = nullptr)
{
	std::array<uint8_t, 64> reply{};
	uint32_t current;
	receiver.peekFeature(slot, reply.data(), current);
	if (version)
		*version = current;
	return reply;
}

std::array<uint8_t, 64> diagnostics(sc::Receiver &receiver)
{
	const uint8_t request[] = {0xf8, 0};
	REQUIRE(receiver.feature(0, request, sizeof(request)));
	return peek(receiver, 0);
}

std::array<uint8_t, 64> disconnectDiagnostics(sc::Receiver &receiver, unsigned slot = 0)
{
	const uint8_t request[] = {0xfb, 0};
	REQUIRE(receiver.feature(slot, request, sizeof(request)));
	auto reply = peek(receiver, slot);
	REQUIRE(reply[0] == 0xfb && reply[1] == 62 && reply[2] == 1);
	REQUIRE(reply[3] == radio::FIRMWARE_REVISION && reply[63] == radio::FIRMWARE_REVISION);
	requireZero(reply.data() + 24, 8); // USB patches these two counters.
	for (unsigned i = 0; i < sc::SLOT_COUNT; ++i)
		REQUIRE(reply[32 + 7 * i + 6] == 0); // No received-header disclosure.
	requireZero(reply.data() + 61, 2);
	return reply;
}

void requireDisconnectCounts(const std::array<uint8_t, 64> &reply,
			     const std::array<uint32_t, 5> &expected)
{
	for (unsigned i = 0; i < expected.size(); ++i)
		REQUIRE(get32(reply.data() + 4 + 4 * i) == expected[i]);
}

struct Peer {
	uint8_t id[8]{}, key[16]{}, iv[8]{};
	unsigned slot = 0;
	uint64_t up = 0, down = 0;

	sc::Packet packet(unsigned type, const uint8_t *body, unsigned size)
	{
		++up;
		uint8_t header = uint8_t(type | ((up & 7) << 4) | ((down & 1) << 2));
		sc::Packet wire{};
		// Peer-side encryption never uses the receiver's fault injection hook.
		REQUIRE(sc::seal(aesBlock, key, iv, up, false, header, body, size, wire));
		return wire;
	}

	void send(sc::Receiver &receiver, unsigned type, const uint8_t *body, unsigned size)
	{
		receiver.receive(slot + 2, packet(type, body, size));
	}

	std::vector<uint8_t> read(sc::Receiver &receiver, unsigned expected_type)
	{
		const uint8_t hello[] = {4};
		send(receiver, 1, hello, sizeof(hello));
		sc::Packet wire = receiver.acknowledgement(slot + 2, unsigned(down & 1));
		REQUIRE(wire.size > 1 && unsigned(wire.data[0] & 3) == expected_type);
		REQUIRE(((wire.data[0] >> 2) & 1) == ((down + 1) & 1));
		uint8_t body[sc::MAX_PLAIN];
		unsigned size = 0;
		REQUIRE(sc::open(aesBlock, key, iv, down + 1, true, wire, body, size));
		++down;
		send(receiver, 1, hello, sizeof(hello));
		receiver.service();
		return std::vector<uint8_t>(body, body + size);
	}
};

Peer joinPeer(sc::Receiver &receiver, unsigned serial = 0, const sc::Bond *bond = nullptr)
{
	Peer peer;
	if (bond) {
		std::memcpy(peer.id, bond->id, 8);
		std::memcpy(peer.key, bond->key, 16);
		sc::Packet lookup{};
		lookup.size = 9;
		std::memcpy(lookup.data + 1, peer.id, 8);
		receiver.receive(1, lookup);
	} else {
		for (unsigned i = 0; i < 8; ++i)
			peer.id[i] = uint8_t(0x40 + 8 * serial + i);
		std::memcpy(peer.key, sc::BOOTSTRAP_KEY, 16);
		receiver.pairing(true);
	}
	sc::Packet advertisement = receiver.acknowledgement(1, 0);
	REQUIRE(advertisement.size == 22 && advertisement.data[0] == 4);
	peer.slot = (advertisement.data[9] & 7) - 2;
	REQUIRE(peer.slot < sc::SLOT_COUNT);
	std::memcpy(peer.iv, advertisement.data + 1, 4);
	for (unsigned i = 0; i < 4; ++i)
		peer.iv[i + 4] = uint8_t(0xb0 + serial + i);
	uint8_t body[17];
	std::memcpy(body, receiver.database().address, 5);
	std::memcpy(body + 5, peer.id, 8);
	std::memcpy(body + 13, peer.iv + 4, 4);
	sc::Packet wire{};
	REQUIRE(sc::seal(aesBlock, peer.key, advertisement.data + 1, 0, false,
			 bond ? 2 : 1, body, sizeof(body), wire));
	receiver.receive(1, wire);
	REQUIRE(receiver.connected(peer.slot));
	REQUIRE(receiver.pending(peer.slot) == (bond == nullptr));
	return peer;
}

void requireEmptyDownlink(const sc::Receiver &receiver, const Peer &peer)
{
	REQUIRE(receiver.acknowledgement(peer.slot + 2, unsigned(peer.down & 1)).size == 1);
}

void drainInitial(sc::Receiver &receiver, Peer &peer)
{
	receiver.service();
	const std::vector<uint8_t> info = {4, 0, 0, 0, 0x55, 0xf7, 0x16, 0x0e};
	REQUIRE(peer.read(receiver, 1) == info);
	auto count = peer.read(receiver, 1);
	REQUIRE(count.size() == 2 && count[0] == 3);
	requireEmptyDownlink(receiver, peer);
}

// Independent table recovered from D:29b0..29ea, including the 95/97 distinction.
const uint8_t fallback_cases[][2] = {
	{0x91, 2}, {0x92, 2}, {0x93, 2}, {0x94, 0},
	{0x95, 2}, {0x97, 0}, {0x98, 2}, {0x99, 2},
};

std::array<uint8_t, 64> expectedFallback(uint8_t command)
{
	std::array<uint8_t, 64> reply{};
	reply[0] = command;
	for (const auto &entry : fallback_cases)
		if (entry[0] == command) {
			reply[0] = 0x94;
			reply[1] = 2;
			reply[2] = entry[1];
		}
	if (command == 0xa1) {
		reply[1] = 34;
		const uint8_t tag[] = {0x55, 0xf7, 0x16, 0x0e};
		std::memcpy(reply.data() + 2, tag, sizeof(tag));
		std::memcpy(reply.data() + 6, "2836576294", 10);
		std::memcpy(reply.data() + 16, "SC2015 XIAO", 11);
	} else if (command == 0xb4) {
		reply[1] = 1;
		reply[2] = 1;
	}
	return reply;
}

void testClassificationAndStartup()
{
	const uint8_t local[] = {
		0x90, 0x96, 0x9b, 0xa1, 0xad, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4,
		0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe,
	};
	for (unsigned command = 0; command < 256; ++command) {
		bool expected = false;
		for (uint8_t value : local)
			expected |= command == value;
		REQUIRE(sc::localFeature(uint8_t(command)) == expected);
	}
	static_assert(sc::localFeature(0x90) && !sc::localFeature(0xa2) &&
		      sc::localFeature(0xfe) && !sc::localFeature(0xff), "local command boundaries");
	Fixture fixture;
	auto &receiver = fixture.receiver;
	for (unsigned slot = 0; slot < sc::SLOT_COUNT; ++slot) {
		uint32_t version = 0;
		auto reply = peek(receiver, slot, &version);
		REQUIRE(version == 1);
		requireZero(reply.data(), unsigned(reply.size()));
		receiver.consumeFeature(slot, version);
		REQUIRE(peek(receiver, slot, &version) == reply && version == 1);
	}
	uint32_t invalid_version = 1;
	auto invalid = peek(receiver, sc::SLOT_COUNT, &invalid_version);
	REQUIRE(invalid_version == 0);
	requireZero(invalid.data(), unsigned(invalid.size()));
	const uint8_t valid[] = {0x87, 0};
	const uint8_t short_frame[] = {0xad, 2, 1};
	uint8_t oversized[65] = {0x87, 62};
	REQUIRE(!receiver.canQueueFeature(sc::SLOT_COUNT, valid, sizeof(valid)));
	REQUIRE(!receiver.canQueueFeature(0, nullptr, 2));
	REQUIRE(!receiver.canQueueFeature(0, valid, 0));
	REQUIRE(!receiver.canQueueFeature(0, valid, 1));
	REQUIRE(!receiver.canQueueFeature(0, short_frame, sizeof(short_frame)));
	REQUIRE(!receiver.canQueueFeature(0, oversized, sizeof(oversized)));
	oversized[1] = 63;
	REQUIRE(!receiver.canQueueFeature(0, oversized, 64));
	REQUIRE(!receiver.feature(0, short_frame, sizeof(short_frame)));
	REQUIRE(!receiver.feature(0, nullptr, 2));
	REQUIRE(!receiver.pairing() && capture.reports.empty());
}

void testFallbackDispatch()
{
	// All 64 selectors, each on a fresh offline receiver so side effects cannot
	// obscure the GET dispatch. Rejected local commands still set last_command.
	for (unsigned command = 0x80; command <= 0xbf; ++command) {
		Fixture fixture;
		uint8_t request[64] = {uint8_t(command), 0};
		bool accepted = command != 0x90 && command != 0xaf &&
				command != 0xb0 && command != 0xb1;
		REQUIRE(fixture.receiver.canQueueFeature(0, request, sizeof(request)));
		REQUIRE(fixture.receiver.feature(0, request, sizeof(request)) == accepted);
		REQUIRE(peek(fixture.receiver, 0) == expectedFallback(uint8_t(command)));
	}
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	for (const auto &entry : fallback_cases) {
		uint8_t request[] = {entry[0], 0};
		REQUIRE(!sc::localFeature(entry[0]) && !sc::featureQuery(entry[0]));
		REQUIRE(receiver.feature(peer.slot, request, sizeof(request)));
		receiver.service();
		REQUIRE(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + 2));
		REQUIRE(peek(receiver, peer.slot) == expectedFallback(entry[0]));
		const uint8_t response[] = {entry[0], 2, 0x7b, 0x55};
		peer.send(receiver, 3, response, sizeof(response));
		uint32_t version;
		auto reply = peek(receiver, peer.slot, &version);
		REQUIRE(!std::memcmp(reply.data(), response, sizeof(response)));
		requireZero(reply.data() + sizeof(response), 64 - unsigned(sizeof(response)));
		receiver.consumeFeature(peer.slot, version);
		REQUIRE(peek(receiver, peer.slot) == expectedFallback(entry[0]));
	}
}

void testLocalCommandsAndParameters()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	uint8_t boot[] = {0x90, 4, 'z', 'o', 't', '!'};
	uint8_t entropy[18] = {0x96, 16};
	const uint8_t factory[] = {0x9b, 4, 'T', 'E', 'S', 'T'};
	uint8_t raw_factory[64] = {0x9b, 'T', 'E', 'S', 'T'};
	REQUIRE(receiver.canQueueFeature(0, boot, sizeof(boot)));
	REQUIRE(receiver.canQueueFeature(0, entropy, sizeof(entropy)));
	REQUIRE(!receiver.feature(0, boot, sizeof(boot)));
	REQUIRE(receiver.feature(0, entropy, sizeof(entropy)));
	REQUIRE(receiver.feature(0, factory, sizeof(factory)));
	REQUIRE(!receiver.feature(0, raw_factory, sizeof(raw_factory)));
	REQUIRE(!receiver.takeBootloaderRequest());
	receiver.service();
	requireEmptyDownlink(receiver, peer);

	const uint8_t enable[] = {0xad, 2, 0xff, 0x7f};
	const uint8_t disable[] = {0xad, 2, 0, 0xff};
	const uint8_t noop0[] = {0xad, 0};
	const uint8_t noop1[] = {0xad, 1, 1};
	const uint8_t noop3[] = {0xad, 3, 1, 2, 3};
	REQUIRE(receiver.feature(0, noop0, sizeof(noop0)) && !receiver.pairing());
	REQUIRE(receiver.feature(0, noop1, sizeof(noop1)) && !receiver.pairing());
	REQUIRE(receiver.feature(0, noop3, sizeof(noop3)) && !receiver.pairing());
	REQUIRE(receiver.feature(0, enable, sizeof(enable)) && receiver.pairing());
	REQUIRE(receiver.feature(0, noop0, sizeof(noop0)) && receiver.pairing());
	REQUIRE(receiver.feature(0, disable, sizeof(disable)) && !receiver.pairing());

	REQUIRE(receiver.desktopEnabled());
	uint8_t desktop_request[] = {0xb1, 2, 0, 0};
	REQUIRE(receiver.feature(0, desktop_request, sizeof(desktop_request)));
	REQUIRE(!receiver.desktopEnabled());
	unsigned desktop_calls = capture.desktop_calls;
	desktop_request[2] = 1;
	desktop_request[3] = 0xff;
	REQUIRE(receiver.feature(0, desktop_request, sizeof(desktop_request)));
	REQUIRE(!receiver.desktopEnabled() && capture.desktop_calls == desktop_calls);
	desktop_request[2] = 0;
	desktop_request[3] = 2;
	REQUIRE(receiver.feature(0, desktop_request, sizeof(desktop_request)));
	REQUIRE(receiver.desktopEnabled());
	desktop_calls = capture.desktop_calls;
	desktop_request[2] = 0xff;
	desktop_request[3] = 0;
	REQUIRE(receiver.feature(0, desktop_request, sizeof(desktop_request)));
	REQUIRE(receiver.desktopEnabled() && capture.desktop_calls == desktop_calls);
	const uint8_t bad_selector_length[] = {0xb1, 1, 1};
	REQUIRE(!receiver.feature(0, bad_selector_length, sizeof(bad_selector_length)));
	REQUIRE(!receiver.feature(0, desktop_request, 3));
	REQUIRE(receiver.desktopEnabled() && capture.desktop_calls == desktop_calls);
	receiver.service();
	requireEmptyDownlink(receiver, peer);

	const uint8_t uf2[] = {0xf9, 4, 'U', 'F', '2', '!'};
	REQUIRE(receiver.feature(0, uf2, sizeof(uf2)));
	uint32_t version;
	auto reply = peek(receiver, 0, &version);
	REQUIRE(reply[0] == 0xf9 && reply[1] == 2 && reply[2] == 0 && reply[3] == 1);
	REQUIRE(!receiver.takeBootloaderRequest());
	receiver.consumeFeature(0, version);
	REQUIRE(receiver.takeBootloaderRequest());
	REQUIRE(!receiver.takeBootloaderRequest());
	requireEmptyDownlink(receiver, peer);

	// Capacity is not acceptance: import blocks a real AD operation, but an
	// ignored parameter length remains a no-op even while import is staged.
	Fixture importing;
	const uint8_t stage[] = {0xf3, 6, 0x28, 0x36, 0x57, 0x62, 0x94, 0};
	REQUIRE(importing.receiver.feature(0, stage, sizeof(stage)));
	REQUIRE(peek(importing.receiver, 0)[2] == 0);
	REQUIRE(importing.receiver.canQueueFeature(0, enable, sizeof(enable)));
	REQUIRE(!importing.receiver.feature(0, enable, sizeof(enable)));
	REQUIRE(importing.receiver.feature(0, noop1, sizeof(noop1)));
	REQUIRE(!importing.receiver.pairing());
}

void testCapacityAndRepeatedWrites()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	uint8_t short_write[64] = {0x87, 0};
	std::memset(short_write + 2, 0x5a, 62); // Padding must not consume three fragments.
	for (unsigned i = 0; i < 10; ++i)
		REQUIRE(receiver.feature(0, short_write, sizeof(short_write)));
	uint8_t large[64] = {0x87, 52}; // 54 total bytes: two fragments.
	REQUIRE(receiver.canQueueFeature(0, large, sizeof(large)));
	large[1] = 53; // 55 total bytes: three fragments.
	REQUIRE(!receiver.canQueueFeature(0, large, sizeof(large)));
	REQUIRE(!receiver.feature(0, large, sizeof(large)));
	REQUIRE(receiver.statistics().queue_full == 1); // Neither free fragment was consumed.
	REQUIRE(receiver.feature(0, short_write, sizeof(short_write)));
	large[1] = 25;
	REQUIRE(receiver.canQueueFeature(0, large, sizeof(large)));
	large[1] = 26;
	REQUIRE(!receiver.canQueueFeature(0, large, sizeof(large)));
	REQUIRE(receiver.feature(0, short_write, sizeof(short_write)));
	large[1] = 62;
	uint32_t before_version;
	auto before_reply = peek(receiver, 0, &before_version);
	unsigned before_aes = aes_fault.calls, before_random = capture.random_byte;
	unsigned before_desktop = capture.desktop_calls;
	size_t before_reports = capture.reports.size();
	const uint8_t local[] = {0x90, 0};
	const uint8_t diagnostic[] = {0xfb, 0};
	for (unsigned i = 0; i < 20; ++i) {
		REQUIRE(!receiver.canQueueFeature(0, short_write, sizeof(short_write)));
		REQUIRE(!receiver.canQueueFeature(0, large, sizeof(large)));
		REQUIRE(receiver.canQueueFeature(0, local, sizeof(local)));
		REQUIRE(receiver.canQueueFeature(0, diagnostic, sizeof(diagnostic)));
		REQUIRE(receiver.canQueueFeature(1, large, sizeof(large)));
	}
	uint32_t after_version;
	REQUIRE(peek(receiver, 0, &after_version) == before_reply);
	REQUIRE(after_version == before_version && receiver.statistics().queue_full == 1);
	REQUIRE(aes_fault.calls == before_aes && capture.random_byte == before_random);
	REQUIRE(capture.desktop_calls == before_desktop && capture.reports.size() == before_reports);
	REQUIRE(capture.saves == 0);
	// Keep the protocol-level rejection for W, D and Q. USB chooses wire policy.
	const uint8_t blocked[] = {0x8f, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0x87, 0x83};
	for (uint8_t command : blocked) {
		const uint8_t request[] = {command, 0};
		REQUIRE(!receiver.canQueueFeature(0, request, sizeof(request)));
		REQUIRE(!receiver.feature(0, request, sizeof(request)));
	}
	REQUIRE(receiver.statistics().queue_full == 1 + sizeof(blocked));
	const uint8_t query[] = {0x83, 0};
	REQUIRE(receiver.queryStatus(0, query, sizeof(query)) == sc::QueryStatus::None);
	auto status = disconnectDiagnostics(receiver);
	REQUIRE(status[34] == 12 && status[60] == 1);
	REQUIRE(receiver.statistics().queue_full == 1 + sizeof(blocked));
	REQUIRE(aes_fault.calls == before_aes && capture.random_byte == before_random);
	REQUIRE(capture.saves == 0 && capture.reports.size() == before_reports);
	receiver.service();
	REQUIRE(receiver.canQueueFeature(0, short_write, sizeof(short_write)));
	REQUIRE(!receiver.canQueueFeature(0, large, sizeof(large)));
	for (unsigned i = 0; i < 12; ++i)
		REQUIRE(peer.read(receiver, 3) == std::vector<uint8_t>(short_write, short_write + 2));
	requireEmptyDownlink(receiver, peer);

	for (unsigned i = 2; i < sizeof(large); ++i)
		large[i] = uint8_t(i);
	for (unsigned repeat = 0; repeat < 2; ++repeat) {
		REQUIRE(receiver.canQueueFeature(0, large, sizeof(large)));
		REQUIRE(receiver.feature(0, large, sizeof(large)));
		receiver.service();
		std::vector<uint8_t> received;
		for (unsigned part = 0; part < 3; ++part) {
			auto bytes = peer.read(receiver, part == 2 ? 3 : 2);
			REQUIRE(bytes.size() == (part == 2 ? 10u : 27u));
			received.insert(received.end(), bytes.begin(), bytes.end());
		}
		REQUIRE(received == std::vector<uint8_t>(large, large + sizeof(large)));
	}
	const uint8_t pulse[] = {0x8f, 7, 0, 0x2c, 1, 0x90, 1, 2, 0};
	for (unsigned i = 0; i < 3; ++i)
		REQUIRE(receiver.feature(0, pulse, sizeof(pulse)));
	receiver.service();
	for (unsigned i = 0; i < 3; ++i)
		REQUIRE(peer.read(receiver, 3) == std::vector<uint8_t>(pulse, pulse + sizeof(pulse)));
	requireEmptyDownlink(receiver, peer);
}

void testQueryExtensions()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	drainInitial(receiver, peer);
	const uint8_t commands[] = {0x83, 0xae, 0xba};
	for (uint8_t command : commands) {
		const uint8_t request[] = {command, 1, 1};
		REQUIRE(sc::featureQuery(command) && !sc::localFeature(command));
		REQUIRE(receiver.feature(0, request, sizeof(request)));
		REQUIRE(receiver.queryStatus(0, request, sizeof(request)) == sc::QueryStatus::Pending);
		uint32_t version;
		auto placeholder = peek(receiver, 0, &version);
		REQUIRE(placeholder[0] == command && placeholder[1] == 0);
		receiver.consumeFeature(0, version);
		for (unsigned i = 0; i < 3; ++i)
			REQUIRE(receiver.feature(0, request, sizeof(request)));
		receiver.service();
		REQUIRE(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + sizeof(request)));
		requireEmptyDownlink(receiver, peer);
		const uint8_t response[] = {command, 2, 1, 0x42};
		peer.send(receiver, 3, response, sizeof(response));
		REQUIRE(receiver.queryStatus(0, request, sizeof(request)) == sc::QueryStatus::Ready);
		const uint8_t identity[] = {0xa1, 0};
		REQUIRE(receiver.feature(0, identity, sizeof(identity)));
		REQUIRE(peek(receiver, 0) == expectedFallback(0xa1));
		REQUIRE(receiver.feature(0, request, sizeof(request)));
		auto reply = peek(receiver, 0, &version);
		REQUIRE(!std::memcmp(reply.data(), response, sizeof(response)));
		receiver.consumeFeature(0, version);
		REQUIRE(receiver.queryStatus(0, request, sizeof(request)) == sc::QueryStatus::None);
		requireEmptyDownlink(receiver, peer);
	}
	// The probe reports physical FIFO space, even when feature() can reuse a
	// pending query. It must not mutate or replace QueryStatus for the USB owner.
	const uint8_t write[] = {0x87, 0};
	for (unsigned i = 0; i < 11; ++i)
		REQUIRE(receiver.feature(0, write, sizeof(write)));
	const uint8_t query[] = {0xae, 1, 1};
	REQUIRE(receiver.feature(0, query, sizeof(query)));
	REQUIRE(!receiver.canQueueFeature(0, query, sizeof(query)));
	REQUIRE(receiver.queryStatus(0, query, sizeof(query)) == sc::QueryStatus::Pending);
	REQUIRE(receiver.feature(0, query, sizeof(query)));
	REQUIRE(receiver.statistics().queue_full == 0);
	receiver.service();
	for (unsigned i = 0; i < 11; ++i)
		REQUIRE(peer.read(receiver, 3) == std::vector<uint8_t>(write, write + sizeof(write)));
	REQUIRE(peer.read(receiver, 3) == std::vector<uint8_t>(query, query + sizeof(query)));
	requireEmptyDownlink(receiver, peer);
}

void requireNeutral(const Report &report, unsigned slot, uint32_t sequence)
{
	REQUIRE(report.slot == slot);
	REQUIRE(report.bytes[0] == 1 && report.bytes[1] == 0 &&
		report.bytes[2] == 1 && report.bytes[3] == 60);
	REQUIRE(get32(report.bytes.data() + 4) == sequence);
	requireZero(report.bytes.data() + 8, 56);
}

void testInputReset()
{
	sc::Database db = database();
	for (unsigned slot = 0; slot < sc::SLOT_COUNT; ++slot) {
		auto &bond = db.bonds[slot];
		bond.used = true;
		for (unsigned i = 0; i < 8; ++i)
			bond.id[i] = uint8_t(0x40 + slot * 8 + i);
		for (unsigned i = 0; i < 16; ++i)
			bond.key[i] = uint8_t(0x80 + slot * 16 + i);
	}
	Fixture fixture(db);
	auto &receiver = fixture.receiver;
	Peer peers[sc::SLOT_COUNT];
	for (unsigned slot = 0; slot < sc::SLOT_COUNT; ++slot)
		peers[slot] = joinPeer(receiver, slot, &db.bonds[slot]);
	for (Peer &peer : peers)
		drainInitial(receiver, peer);
	capture.reports.clear();
	for (Peer &peer : peers) {
		// Populate all 26 persistent extension bytes, then the base state and
		// keyboard/mouse. A later empty input must not resurrect extension data.
		uint8_t tail[27] = {0, 0, 0x20};
		std::memset(tail + 3, 0x65, 24);
		peer.send(receiver, 0, tail, sizeof(tail));
		const uint8_t end[] = {0, 0, 24, 0x66, 0x67};
		peer.send(receiver, 0, end, sizeof(end));
		uint8_t state[25] = {0x70, 0x80}; // state16, keyboard3, mouse4
		std::memset(state + 2, 0x31, 16);
		state[18] = uint8_t(4 + peer.slot);
		state[19] = uint8_t(1u << peer.slot);
		state[20] = uint8_t(20 + peer.slot);
		state[21] = 1;
		state[22] = 2;
		state[23] = uint8_t(1u << peer.slot);
		state[24] = 3;
		peer.send(receiver, 0, state, sizeof(state));
		for (unsigned i = 8; i < 50; ++i)
			REQUIRE(capture.reports.back().bytes[i] != 0);
	}
	REQUIRE(capture.keyboard[0] == 15 && capture.mouse[0] == 15);
	uint32_t sequence = get32(capture.reports.back().bytes.data() + 4);
	sc::Packet downlinks[sc::SLOT_COUNT];
	for (Peer &peer : peers) {
		const uint8_t request[] = {0x87, 1, uint8_t(peer.slot)};
		REQUIRE(receiver.feature(peer.slot, request, sizeof(request)));
	}
	receiver.service();
	for (const Peer &peer : peers) {
		// Compare serialized cache across reset, not the ISR admission decision.
		downlinks[peer.slot] = receiver.acknowledgement(peer.slot + 2, unsigned(peer.down & 1), false);
		REQUIRE(downlinks[peer.slot].size > 1);
	}
	uint8_t challenge[8];
	std::memcpy(challenge, receiver.challenge(), 8);
	uint8_t pipes = receiver.activePipes();
	uint32_t authenticated = receiver.statistics().authenticated;
	unsigned desktop_calls = capture.desktop_calls;
	capture.reports.clear();
	receiver.resetInputs();
	REQUIRE(capture.reports.size() == sc::SLOT_COUNT);
	for (unsigned slot = 0; slot < sc::SLOT_COUNT; ++slot)
		requireNeutral(capture.reports[slot], slot, ++sequence);
	REQUIRE(capture.desktop_calls == desktop_calls + 1 && receiver.desktopEnabled());
	requireZero(capture.keyboard.data(), unsigned(capture.keyboard.size()));
	requireZero(capture.mouse.data(), unsigned(capture.mouse.size()));
	REQUIRE(receiver.activePipes() == pipes && receiver.statistics().authenticated == authenticated);
	REQUIRE(!std::memcmp(challenge, receiver.challenge(), 8) && capture.saves == 0);
	requireDatabase(receiver.database(), db);
	for (Peer &peer : peers) {
		REQUIRE(receiver.connected(peer.slot) && !receiver.pending(peer.slot));
		sc::Packet after = receiver.acknowledgement(peer.slot + 2, unsigned(peer.down & 1), false);
		REQUIRE(after.size == downlinks[peer.slot].size);
		REQUIRE(!std::memcmp(after.data, downlinks[peer.slot].data, after.size));
		const std::vector<uint8_t> expected = {0x87, 1, uint8_t(peer.slot)};
		REQUIRE(peer.read(receiver, 3) == expected); // Same keys, counters and FIFO.
		const uint8_t empty[] = {0, 0};
		peer.send(receiver, 0, empty, sizeof(empty));
		requireNeutral(capture.reports.back(), peer.slot, ++sequence);
	}
	capture.reports.clear();
	receiver.disconnect(0);
	REQUIRE(capture.reports.size() == 2);
	const auto &event = capture.reports[1];
	REQUIRE(event.slot == 0 && event.bytes[0] == 1 && event.bytes[1] == 0 &&
		event.bytes[2] == 3 && event.bytes[3] == 1 && event.bytes[4] == 1);
	requireNeutral(capture.reports[0], 0, ++sequence);
	REQUIRE(!receiver.connected(0) && receiver.connected(1));
	requireDatabase(receiver.database(), db);

	// Reset also emits four reports for offline slots and preserves host gates.
	Fixture offline;
	offline.receiver.pairing(true);
	const uint8_t disable[] = {0xb1, 2, 0, 0};
	REQUIRE(offline.receiver.feature(0, disable, sizeof(disable)));
	offline.receiver.resetInputs();
	REQUIRE(offline.receiver.pairing() && !offline.receiver.desktopEnabled());
	REQUIRE(capture.reports.size() == sc::SLOT_COUNT);
	for (unsigned slot = 0; slot < sc::SLOT_COUNT; ++slot)
		requireNeutral(capture.reports[slot], slot, slot + 1);
}

void testSessionGenerationsAndDisconnectDiagnostics()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	const uint8_t disconnect[] = {0xb2, 0};
	REQUIRE(receiver.sessionGeneration(sc::SLOT_COUNT) == 0);
	REQUIRE(!receiver.featureForSession(sc::SLOT_COUNT, 0, disconnect, sizeof(disconnect)));
	for (unsigned i = 0; i < sc::SLOT_COUNT; ++i)
		REQUIRE(receiver.sessionGeneration(i) == 1);
	auto initial = disconnectDiagnostics(receiver);
	requireZero(initial.data() + 4, 59);
	const uint32_t offline_generation = receiver.sessionGeneration(0);
	unsigned before_aes = aes_fault.calls, before_random = capture.random_byte;
	REQUIRE(receiver.featureForSession(0, offline_generation, disconnect, sizeof(disconnect)));
	REQUIRE(receiver.sessionGeneration(0) == offline_generation);
	REQUIRE(aes_fault.calls == before_aes && capture.random_byte == before_random);
	REQUIRE(disconnectDiagnostics(receiver) == initial);

	Peer peer = joinPeer(receiver);
	const uint32_t live_generation = receiver.sessionGeneration(peer.slot);
	REQUIRE(peer.slot == 0 && live_generation != offline_generation);
	REQUIRE(receiver.sessionGeneration(1) == 1);
	uint32_t version;
	auto before = peek(receiver, 0, &version);
	before_aes = aes_fault.calls;
	before_random = capture.random_byte;
	size_t before_reports = capture.reports.size();
	// B2 captured offline must not acquire the newly admitted session. Stale
	// dispatch must not even replace the current Feature reply/version.
	REQUIRE(!receiver.featureForSession(0, offline_generation, disconnect, sizeof(disconnect)));
	REQUIRE(!receiver.featureForSession(0, 0, disconnect, sizeof(disconnect)));
	uint32_t after_version;
	REQUIRE(peek(receiver, 0, &after_version) == before && after_version == version);
	REQUIRE(receiver.connected(0) && receiver.sessionGeneration(0) == live_generation);
	REQUIRE(aes_fault.calls == before_aes && capture.random_byte == before_random);
	REQUIRE(capture.reports.size() == before_reports && capture.saves == 0);

	receiver.service(); // Two prepared controls, then an authenticated parity0 RX.
	const uint8_t sample[] = {0, 0};
	peer.send(receiver, 0, sample, sizeof(sample));
	const uint8_t write[] = {0x87, 0};
	for (unsigned i = 0; i < 3; ++i)
		REQUIRE(receiver.featureForSession(0, live_generation, write, sizeof(write)));
	REQUIRE(receiver.sessionGeneration(0) == live_generation);
	auto live = disconnectDiagnostics(receiver);
	const uint8_t expected[] = {0, 41, 3, 0x0b, 1, 2, 0};
	REQUIRE(!std::memcmp(live.data() + 32, expected, sizeof(expected)) && live[60] == 1);
	requireDisconnectCounts(live, {0, 0, 0, 0, 0});
	REQUIRE(receiver.featureForSession(0, live_generation, disconnect, sizeof(disconnect)));
	REQUIRE(!receiver.connected(0) && receiver.sessionGeneration(0) != live_generation);
	auto offline = disconnectDiagnostics(receiver);
	requireDisconnectCounts(offline, {0, 1, 0, 0, 0});
	REQUIRE(offline[32] == uint8_t(sc::DisconnectReason::Host) && offline[60] == 0);
	REQUIRE(!std::memcmp(offline.data() + 33, live.data() + 33, 5));
	const uint32_t disconnected_generation = receiver.sessionGeneration(0);
	REQUIRE(!receiver.featureForSession(0, live_generation, disconnect, sizeof(disconnect)));
	receiver.disconnect(0, sc::DisconnectReason::Erase); // Offline is not another event.
	receiver.disconnect(sc::SLOT_COUNT);
	receiver.service();
	for (unsigned i = 0; i < 16; ++i) receiver.hopTick();
	REQUIRE(receiver.sessionGeneration(0) == disconnected_generation);
	REQUIRE(disconnectDiagnostics(receiver, 3) == offline);

	peer = joinPeer(receiver);
	REQUIRE(peer.slot == 0 && receiver.sessionGeneration(0) != disconnected_generation);
	REQUIRE(!receiver.featureForSession(0, disconnected_generation, disconnect, sizeof(disconnect)));
	auto rejoined = disconnectDiagnostics(receiver);
	REQUIRE(rejoined[32] == uint8_t(sc::DisconnectReason::Host) && rejoined[60] == 1);
	REQUIRE(rejoined[33] == 41);
	requireZero(rejoined.data() + 34, 5); // Current session, not the old queue/counters.
	requireDisconnectCounts(rejoined, {0, 1, 0, 0, 0});
	uint32_t old_generation = receiver.sessionGeneration(0);
	uint32_t new_slot_generation = receiver.sessionGeneration(1);
	Peer replacement = joinPeer(receiver); // Same authenticated ID, advertised free slot1.
	REQUIRE(replacement.slot == 1 && !receiver.connected(0) && receiver.connected(1));
	REQUIRE(receiver.sessionGeneration(0) != old_generation);
	REQUIRE(receiver.sessionGeneration(1) != new_slot_generation);
	REQUIRE(receiver.sessionGeneration(2) == 1 && receiver.sessionGeneration(3) == 1);
	REQUIRE(!receiver.featureForSession(0, old_generation, disconnect, sizeof(disconnect)));
	auto replaced = disconnectDiagnostics(receiver);
	requireDisconnectCounts(replaced, {0, 1, 1, 0, 0});
	REQUIRE(replaced[32] == uint8_t(sc::DisconnectReason::Replaced) && replaced[60] == 2);
	REQUIRE(!std::memcmp(replaced.data() + 33, rejoined.data() + 33, 5));
	REQUIRE(replaced[39] == uint8_t(sc::DisconnectReason::None));
	new_slot_generation = receiver.sessionGeneration(1);
	receiver.disconnect(1); // Existing direct callers retain the Host default.
	REQUIRE(receiver.sessionGeneration(1) != new_slot_generation);
	auto final = disconnectDiagnostics(receiver);
	requireDisconnectCounts(final, {0, 2, 1, 0, 0});
	REQUIRE(final[39] == uint8_t(sc::DisconnectReason::Host) && final[60] == 0);
	REQUIRE(capture.saves == 0);
	auto reconnect = diagnostics(receiver);
	REQUIRE(get32(reconnect.data() + 28) == 3); // All three joins remain new_pairs.
	REQUIRE(get32(reconnect.data() + 52) == 3); // Existing unsaved-disconnect semantics.
}

void testWatchdogAndEraseDiagnostics()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	std::array<Peer, sc::SLOT_COUNT> peers;
	for (unsigned i = 0; i < peers.size(); ++i) {
		peers[i] = joinPeer(receiver, i);
		REQUIRE(peers[i].slot == i);
	}
	receiver.service();
	const uint8_t sample[] = {0, 0};
	for (Peer &peer : peers) peer.send(receiver, 0, sample, sizeof(sample));
	auto before = disconnectDiagnostics(receiver);
	REQUIRE(before[60] == 15);
	std::array<uint32_t, sc::SLOT_COUNT> generations;
	for (unsigned i = 0; i < peers.size(); ++i)
		generations[i] = receiver.sessionGeneration(i);
	// Slot2 stops making progress; other sessions remain live throughout expiry.
	for (unsigned tick = 0; tick < 41 * 8; ++tick) {
		receiver.hopTick();
		if (!(tick & 7))
			for (unsigned i : {0u, 1u, 3u})
				peers[i].send(receiver, 0, sample, sizeof(sample));
	}
	auto expired = disconnectDiagnostics(receiver);
	requireDisconnectCounts(expired, {1, 0, 0, 0, 0});
	REQUIRE(expired[60] == 11 && !receiver.connected(2));
	REQUIRE(expired[46] == uint8_t(sc::DisconnectReason::Watchdog) && expired[47] == 0);
	REQUIRE(!std::memcmp(expired.data() + 48, before.data() + 48, 4));
	for (unsigned i = 0; i < peers.size(); ++i)
		REQUIRE((receiver.sessionGeneration(i) != generations[i]) == (i == 2));
	const uint32_t expired_generation = receiver.sessionGeneration(2);
	const uint8_t erase[] = {0xaf, 4, 'w', 'i', 'p', 'e'};
	REQUIRE(receiver.feature(0, erase, sizeof(erase)));
	auto erased = disconnectDiagnostics(receiver);
	requireDisconnectCounts(erased, {1, 0, 0, 0, 3});
	REQUIRE(erased[60] == 0 && capture.saves == 1);
	for (unsigned i = 0; i < peers.size(); ++i) {
		unsigned offset = 32 + 7 * i;
		REQUIRE(erased[offset] == uint8_t(i == 2 ? sc::DisconnectReason::Watchdog :
							 sc::DisconnectReason::Erase));
		REQUIRE(!std::memcmp(erased.data() + offset + 1, expired.data() + offset + 1, 5));
		REQUIRE(receiver.sessionGeneration(i) != generations[i]);
	}
	REQUIRE(receiver.sessionGeneration(2) == expired_generation);
	REQUIRE(receiver.feature(0, erase, sizeof(erase))); // Persistent wipe, no live teardowns.
	REQUIRE(disconnectDiagnostics(receiver) == erased && capture.saves == 2);
	Peer fresh = joinPeer(receiver);
	REQUIRE(fresh.slot == 0);
	auto live = disconnectDiagnostics(receiver);
	requireDisconnectCounts(live, {1, 0, 0, 0, 3});
	REQUIRE(live[32] == uint8_t(sc::DisconnectReason::Erase) && live[33] == 41 && live[60] == 1);
	requireZero(live.data() + 34, 5);
	REQUIRE(!std::memcmp(live.data() + 39, erased.data() + 39, 21));
	REQUIRE(receiver.begin(database(), hooks));
	for (unsigned i = 0; i < peers.size(); ++i)
		REQUIRE(receiver.sessionGeneration(i) == 1);
	auto initialized = disconnectDiagnostics(receiver);
	requireZero(initialized.data() + 4, 59);
}

void testBootstrapSealFailure()
{
	Fixture fixture;
	auto &receiver = fixture.receiver;
	Peer peer = joinPeer(receiver);
	receiver.service(); // An old-key initial ACK can already be on the air.
	sc::Packet old_ack = receiver.acknowledgement(peer.slot + 2, 0, false);
	REQUIRE(old_ack.size > 1);
	uint8_t decoded[sc::MAX_PLAIN];
	unsigned size = 0;
	REQUIRE(sc::open(aesBlock, peer.key, peer.iv, 1, true, old_ack, decoded, size));
	REQUIRE(size == 8 && decoded[0] == 4);
	uint8_t update[17] = {1};
	for (unsigned i = 1; i < sizeof(update); ++i)
		update[i] = uint8_t(0xd0 + i);
	sc::Packet request = peer.packet(1, update, sizeof(update));
	const uint32_t generation = receiver.sessionGeneration(peer.slot);
	auto before_disconnect = disconnectDiagnostics(receiver);

	// Measure production open(), then fail the very next AES block. No magic
	// CCM call count, no failed MIC, and no fault in peer-side seal().
	aes_fault = AesFault{};
	REQUIRE(sc::open(receiverAes, peer.key, peer.iv, peer.up, false, request, decoded, size));
	REQUIRE(size == sizeof(update) && !std::memcmp(decoded, update, size));
	unsigned open_calls = aes_fault.calls;
	REQUIRE(open_calls > 0);
	aes_fault = AesFault{};
	aes_fault.fail_call = open_calls + 1;
	uint32_t authenticated = receiver.statistics().authenticated;
	capture.reports.clear();
	receiver.receive(peer.slot + 2, request);
	REQUIRE(aes_fault.calls == open_calls + 1 && aes_fault.failures == 1);
	REQUIRE(receiver.statistics().authenticated == authenticated + 1);
	REQUIRE(receiver.statistics().mic_failures == 0);
	REQUIRE(!receiver.connected(peer.slot) && !receiver.pending(peer.slot));
	REQUIRE(!(receiver.activePipes() & (1u << (peer.slot + 2))));
	REQUIRE(receiver.acknowledgement(peer.slot + 2, 0).size == 0);
	REQUIRE(receiver.acknowledgement(peer.slot + 2, 1).size == 0);
	REQUIRE(!receiver.joinReplay(peer.slot).active && capture.saves == 0);
	REQUIRE(capture.reports.size() == 2);
	requireNeutral(capture.reports[0], peer.slot, 1);
	REQUIRE(capture.reports[1].bytes[2] == 3 && capture.reports[1].bytes[4] == 1);
	auto status = diagnostics(receiver);
	REQUIRE(get32(status.data() + 32) == 0); // No installed key update.
	REQUIRE(status[60] == 0 && status[61] == 0 && status[62] == 0);
	requireDatabase(receiver.database(), database());
	auto disconnected = disconnectDiagnostics(receiver);
	requireDisconnectCounts(disconnected, {0, 0, 0, 1, 0});
	REQUIRE(disconnected[32] == uint8_t(sc::DisconnectReason::Crypto) && disconnected[60] == 0);
	REQUIRE(!std::memcmp(disconnected.data() + 33, before_disconnect.data() + 33, 5));
	REQUIRE(receiver.sessionGeneration(peer.slot) != generation);
	const uint32_t offline_generation = receiver.sessionGeneration(peer.slot);

	aes_fault = AesFault{};
	authenticated = receiver.statistics().authenticated;
	receiver.receive(peer.slot + 2, request); // Old-key retry cannot resurrect it.
	Peer switched = peer;
	std::memcpy(switched.key, update + 1, 16);
	const uint8_t hello[] = {4};
	switched.send(receiver, 1, hello, sizeof(hello));
	REQUIRE(receiver.statistics().authenticated == authenticated && aes_fault.calls == 0);
	REQUIRE(receiver.acknowledgement(peer.slot + 2, 0).size == 0);
	const uint8_t commit[] = {0xb3, 0};
	REQUIRE(receiver.feature(peer.slot, commit, sizeof(commit)));
	REQUIRE(capture.saves == 0 && capture.reports.back().bytes[4] == 1);

	Peer fresh = joinPeer(receiver);
	REQUIRE(fresh.slot == peer.slot && std::memcmp(fresh.iv, peer.iv, 8));
	REQUIRE(receiver.sessionGeneration(fresh.slot) != offline_generation);
	auto rejoined = disconnectDiagnostics(receiver);
	requireDisconnectCounts(rejoined, {0, 0, 0, 1, 0});
	REQUIRE(rejoined[32] == uint8_t(sc::DisconnectReason::Crypto) && rejoined[33] == 41);
	requireZero(rejoined.data() + 34, 5);
	sc::Packet empty_ack = receiver.acknowledgement(fresh.slot + 2, 0);
	REQUIRE(empty_ack.size == 1 && empty_ack.data[0] == 0);
	receiver.receive(fresh.slot + 2, request); // Previous session/challenge is stale.
	REQUIRE(receiver.connected(fresh.slot) && receiver.statistics().mic_failures == 1);
	empty_ack = receiver.acknowledgement(fresh.slot + 2, 0);
	REQUIRE(empty_ack.size == 1 && empty_ack.data[0] == 0);
	fresh.send(receiver, 1, update, sizeof(update));
	REQUIRE(receiver.connected(fresh.slot) && receiver.pending(fresh.slot));
	// Success still commits candidate=1 after resetTransport(), not before it.
	empty_ack = receiver.acknowledgement(fresh.slot + 2, 1);
	REQUIRE(empty_ack.size == 1 && empty_ack.data[0] == 0x10);
	status = diagnostics(receiver);
	REQUIRE(get32(status.data() + 32) == 1 && status[60] == 1 && status[61] == 1);
	REQUIRE(receiver.feature(fresh.slot, commit, sizeof(commit)) && capture.saves == 0);
	std::memcpy(fresh.key, update + 1, 16);
	fresh.send(receiver, 1, hello, sizeof(hello));
	receiver.service();
	REQUIRE(receiver.connected(fresh.slot) && !receiver.pending(fresh.slot));
	REQUIRE(capture.saves == 1 && capture.stored.bonds[0].used);
	REQUIRE(!std::memcmp(capture.stored.bonds[0].id, fresh.id, 8));
	REQUIRE(!std::memcmp(capture.stored.bonds[0].key, update + 1, 16));
	status = diagnostics(receiver);
	REQUIRE(status[60] == 0 && status[61] == 0 && status[62] == 0);
}

} // namespace

void testProtocolCompatibility()
{
	requirements = 0;
	testClassificationAndStartup();
	testFallbackDispatch();
	testLocalCommandsAndParameters();
	testCapacityAndRepeatedWrites();
	testQueryExtensions();
	testInputReset();
	testSessionGenerationsAndDisconnectDiagnostics();
	testWatchdogAndEraseDiagnostics();
	testBootstrapSealFailure();
	std::printf("PASS: protocol compatibility, session generations, disconnect diagnostics, queue probes, input reset and fail-closed bootstrap (%u requirements)\n",
		    requirements);
}

#undef REQUIRE
