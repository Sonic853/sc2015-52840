#include "protocol.h"
#include "radio_policy.h"
#include "usb.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <device/dcd.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#endif

static unsigned assertions;
void testUsb();
void testUsbControlEdges();
void testProtocolCompatibility();
void testRadioRetention();
void testDesktopUsb();
void testQueueMetrics();
void testQueueUsb(const char *fixture_path);
extern "C" void tud_event_hook_cb(uint8_t, uint32_t, bool);
#define CHECK(value) do { ++assertions; if (!(value)) { \
	std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

static bool aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
#ifdef _WIN32
	BCRYPT_ALG_HANDLE provider = nullptr;
	BCRYPT_KEY_HANDLE handle = nullptr;
	CHECK(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0);
	CHECK(BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
		ULONG(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0) == 0);
	CHECK(BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0,
		const_cast<PUCHAR>(key), 16, 0) == 0);
	ULONG count = 0;
	bool result = BCryptEncrypt(handle, const_cast<PUCHAR>(in), 16, nullptr,
		nullptr, 0, out, 16, &count, 0) == 0 && count == 16;
	BCryptDestroyKey(handle);
	BCryptCloseAlgorithmProvider(provider, 0);
	return result;
#else
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	CHECK(ctx);
	int count = 0;
	bool result = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, out, &count, in, 16) == 1 && count == 16;
	EVP_CIPHER_CTX_free(ctx);
	return result;
#endif
}

static unsigned random_byte;
static bool random_ok = true, storage_ok = true;
static unsigned saves;
static sc::Database stored{};
struct Report { unsigned slot; std::array<uint8_t, 64> bytes; };
static std::vector<Report> reports;
static std::array<uint8_t, 8> keyboard;
static std::array<uint8_t, 4> mouse;

static bool randomBytes(uint8_t *data, size_t size)
{
	if (!random_ok)
		return false;
	for (size_t i = 0; i < size; ++i)
		data[i] = uint8_t(++random_byte);
	return true;
}

static bool persist(const sc::Database &db)
{
	++saves;
	if (storage_ok)
		stored = db;
	return storage_ok;
}

static void input(unsigned slot, const uint8_t report[64])
{
	Report result{};
	result.slot = slot;
	std::memcpy(result.bytes.data(), report, 64);
	reports.push_back(result);
}

static void desktop(const uint8_t keys[8], const uint8_t movement[4])
{
	std::memcpy(keyboard.data(), keys, 8);
	std::memcpy(mouse.data(), movement, 4);
}

static sc::Hooks hooks = {aes, randomBytes, persist, input, desktop};

static sc::Database database()
{
	sc::Database db{};
	const uint8_t address[5] = {0x28, 0x36, 0x57, 0x62, 0x94};
	std::memcpy(db.address, address, 5);
	return db;
}

struct Peer {
	uint8_t id[8]{}, key[16]{}, iv[8]{};
	unsigned slot = 0;
	uint64_t up = 0, down = 0;

	void send(sc::Receiver &receiver, unsigned type, const uint8_t *plain,
		  unsigned size, bool advance = true)
	{
		if (advance)
			++up;
		uint8_t header = uint8_t(type | ((up & 7) << 4) | ((down & 1) << 2));
		sc::Packet packet;
		CHECK(sc::seal(aes, key, iv, up, false, header, plain, size, packet));
		receiver.receive(slot + 2, packet);
	}

	std::vector<uint8_t> read(sc::Receiver &receiver, unsigned expected_type)
	{
		const uint8_t control[8] = {4};
		// An ACK payload is elicited by an uplink carrying the previous phase.
		send(receiver, 1, control, sizeof(control));
		sc::Packet packet = receiver.acknowledgement(slot + 2, unsigned(down & 1));
		CHECK(packet.size > 1);
		CHECK(unsigned(packet.data[0] & 3) == expected_type);
		CHECK(unsigned((packet.data[0] >> 2) & 1) == unsigned((down + 1) & 1));
		uint8_t plain[27];
		unsigned size = 0;
		CHECK(sc::open(aes, key, iv, down + 1, true, packet, plain, size));
		++down;
		send(receiver, 1, control, sizeof(control));
		receiver.service();
		return std::vector<uint8_t>(plain, plain + size);
	}
};

static Peer connect(sc::Receiver &receiver, unsigned serial = 0, bool restore = false,
		    const uint8_t *saved_key = nullptr, bool prepare = true, bool query = true,
		    bool auto_commit = false)
{
	Peer peer;
	for (unsigned i = 0; i < 8; ++i)
		peer.id[i] = uint8_t(0x30 + i + serial * 8);
	std::memcpy(peer.key, restore ? saved_key : sc::BOOTSTRAP_KEY, 16);
	if (restore && query) {
		sc::Packet lookup{};
		lookup.size = 9;
		std::memcpy(lookup.data + 1, peer.id, 8);
		receiver.receive(1, lookup);
	} else if (!restore && (!receiver.pairing() || auto_commit)) {
		receiver.pairing(true, auto_commit);
	}
	sc::Packet ack = receiver.acknowledgement(1, 0);
	CHECK(ack.size == 22 && ack.data[0] == 4);
	peer.slot = (ack.data[9] & 7) - 2;
	CHECK(peer.slot < 4);
	std::memcpy(peer.iv, ack.data + 1, 4);
	for (unsigned i = 0; i < 4; ++i)
		peer.iv[4 + i] = uint8_t(0xa0 + i + serial);
	uint8_t plain[17];
	std::memcpy(plain, receiver.database().address, 5);
	std::memcpy(plain + 5, peer.id, 8);
	std::memcpy(plain + 13, peer.iv + 4, 4);
	sc::Packet join;
	CHECK(sc::seal(aes, peer.key, ack.data + 1, 0, false,
		      restore ? 2 : 1, plain, 17, join));
	receiver.receive(1, join);
	CHECK(receiver.connected(peer.slot));
	CHECK(receiver.pending(peer.slot) == !restore);
	CHECK(!receiver.pairing());
	if (prepare)
		receiver.service();
	return peer;
}

static void drainInitial(sc::Receiver &receiver, Peer &peer)
{
	auto first = peer.read(receiver, 1);
	CHECK(first.size() == 8 && first[0] == 4);
	auto second = peer.read(receiver, 1);
	CHECK(second.size() == 2 && second[0] == 3);
	CHECK(receiver.acknowledgement(peer.slot + 2, unsigned(peer.down & 1)).size == 1);
}

static void testCrypto(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	CHECK(stream.good());
	auto read = [&stream](void *out, size_t size) {
		stream.read(static_cast<char *>(out), std::streamsize(size));
		CHECK(stream.good());
	};
	uint32_t total;
	read(&total, 4);
	CHECK(total == 108);
	for (unsigned test = 0; test < total; ++test) {
		uint8_t key[16], iv[8], direction, header, size, plain[27], wire_size;
		uint64_t counter;
		read(key, 16); read(iv, 8); read(&counter, 8);
		read(&direction, 1); read(&header, 1); read(&size, 1);
		CHECK(size && size <= 27);
		read(plain, size); read(&wire_size, 1);
		sc::Packet expected{};
		expected.size = wire_size;
		CHECK(wire_size == size + 5);
		read(expected.data, wire_size);
		sc::Packet actual;
		CHECK(sc::seal(aes, key, iv, counter, direction != 0, header, plain, size, actual));
		CHECK(actual.size == expected.size);
		CHECK(!std::memcmp(actual.data, expected.data, actual.size));
		uint8_t decrypted[27];
		unsigned length = 0;
		CHECK(sc::open(aes, key, iv, counter, direction != 0, expected, decrypted, length));
		CHECK(length == size && !std::memcmp(plain, decrypted, size));
		for (unsigned i = 1; i < expected.size; ++i) {
			sc::Packet broken = expected;
			broken.data[i] ^= 0x80;
			CHECK(!sc::open(aes, key, iv, counter, direction != 0, broken, decrypted, length));
			CHECK(length == 0);
		}
		CHECK(!sc::open(aes, key, iv, counter, !direction, expected, decrypted, length));
		CHECK(!sc::open(aes, key, iv, (counter + 1) & sc::COUNTER_MASK,
			direction != 0, expected, decrypted, length));
		sc::Packet changed = expected;
		changed.data[0] ^= 0x7c;
		CHECK(sc::open(aes, key, iv, counter, direction != 0, changed, decrypted, length));
		changed.data[0] ^= 1;
		CHECK(!sc::open(aes, key, iv, counter, direction != 0, changed, decrypted, length));
	}
	CHECK(stream.peek() == EOF);
	std::puts("PASS: 108 independent PyCryptodome CCM fixtures, MIC tampering, AAD and direction");
}

static void testHops(const char *path)
{
	std::ifstream stream(path, std::ios::binary);
	for (unsigned i = 0; i < 128; ++i) {
		uint8_t address[5], expected[7], actual[7];
		stream.read(reinterpret_cast<char *>(address), 5);
		stream.read(reinterpret_cast<char *>(expected), 7);
		CHECK(stream.good());
		sc::makeHops(address, actual);
		CHECK(!std::memcmp(expected, actual, 7));
	}
	CHECK(sc::radioBase(sc::DISCOVERY_ADDRESS) == 0x82326aa2);
	CHECK(sc::reverse8(sc::DISCOVERY_ADDRESS[0]) == 0x6a);
	for (uint64_t previous = 0; previous < 2048; ++previous)
		for (uint64_t delta = 0; delta < 8; ++delta)
			CHECK(sc::uplinkCounter(previous, uint8_t(((previous + delta) & 7) << 4)) == previous + delta);
	CHECK(sc::uplinkCounter(sc::COUNTER_MASK, 0) == 0);
	uint32_t deadline = 1500;
	uint8_t index = 0;
	CHECK(sc::advanceHops(1499, deadline, index) == 0);
	CHECK(sc::advanceHops(1500, deadline, index) == 1 && deadline == 3000 && index == 1);
	CHECK(sc::advanceHops(63000, deadline, index) == 41 && deadline == 64500 && index == 0);
	deadline = 0xffffff00u;
	index = 6;
	CHECK(sc::advanceHops(0x600, deadline, index) == 2);
	CHECK(index == 1 && deadline == uint32_t(0xffffff00u + 3000u));
	std::puts("PASS: 128 firmware-derived hop fixtures, address bit order, counter window/wrap");
}

static void testStorage()
{
	sc::Database db = database(), restored{};
	db.bonds[19].used = true;
	for (unsigned i = 0; i < 16; ++i)
		db.bonds[19].key[i] = uint8_t(i + 1);
	uint8_t data[sc::STORE_SIZE];
	CHECK(sc::encodeDatabase(db, 0xffffffff, data));
	uint32_t generation = 0;
	CHECK(sc::decodeDatabase(data, restored, generation));
	CHECK(generation == 0xffffffff && restored.bonds[19].used);
	CHECK(!std::memcmp(db.bonds[19].key, restored.bonds[19].key, 16));
	for (unsigned i = 0; i < sc::STORE_SIZE; ++i) {
		data[i] ^= 1;
		CHECK(!sc::decodeDatabase(data, restored, generation));
		data[i] ^= 1;
	}
	for (unsigned cut = 0; cut < sc::STORE_SIZE; ++cut) {
		uint8_t torn[sc::STORE_SIZE];
		std::memset(torn, 255, sizeof(torn));
		std::memcpy(torn, data, cut);
		CHECK(!sc::decodeDatabase(torn, restored, generation));
	}
	CHECK(sc::generationAfter(0, 0xffffffff));
	CHECK(!sc::generationAfter(0xffffffff, 0));
	CHECK(!sc::generationAfter(2, 2));
	CHECK(sc::crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926);
	std::puts("PASS: all record byte corruptions and torn writes rejected; generation wrap");
}

static void testLink()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	CHECK(receiver.activePipes() == 2);
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	reports.clear();
	uint8_t sample[] = {0x24, 0x20, 0x81, 0x22, 0x33, 0x44, 0x04, 0x03,
			    0x20, 0xaa, 0xbb};
	peer.send(receiver, 0, sample, sizeof(sample));
	CHECK(reports.size() == 1);
	const auto &report = reports.back().bytes;
	CHECK(report[0] == 1 && report[2] == 1 && report[3] == 60);
	CHECK(report[8] == 0x81 && report[9] == 0x22 && report[10] == 0x33);
	CHECK(report[20] == 0x44 && report[24] == 0xaa && report[25] == 0xbb);
	CHECK(keyboard[2] == 4 && mouse[1] == 3);
	unsigned before = unsigned(reports.size());
	peer.send(receiver, 0, sample, sizeof(sample), false);
	CHECK(reports.size() == before);
	CHECK(receiver.statistics().duplicates >= 1);
	uint8_t continuation[] = {0, 0, 1, 0xcc};
	peer.send(receiver, 0, continuation, sizeof(continuation));
	CHECK(reports.back().bytes[24] == 0xaa && reports.back().bytes[25] == 0xcc);
	uint8_t special[] = {0, 0x1f, 4, 2, 0x61, 0x62};
	peer.send(receiver, 0, special, sizeof(special));
	CHECK(reports.back().bytes[2] == 4 && reports.back().bytes[4] == 0x61);
	before = unsigned(reports.size());
	for (unsigned i = 17; i <= 31; ++i) {
		uint8_t malformed[] = {uint8_t(i), 0};
		peer.send(receiver, 0, malformed, sizeof(malformed));
	}
	CHECK(reports.size() == before);
	uint8_t request[64];
	for (unsigned i = 0; i < 64; ++i)
		request[i] = uint8_t(i);
	request[0] = 0x84; request[1] = 62;
	CHECK(receiver.feature(0, request, sizeof(request)));
	CHECK(receiver.feature(0, request, sizeof(request)));
	receiver.service();
	std::vector<uint8_t> received;
	for (unsigned i = 0; i < 3; ++i) {
		auto fragment = peer.read(receiver, i == 2 ? 3 : 2);
		received.insert(received.end(), fragment.begin(), fragment.end());
	}
	CHECK(received.size() == 64 && !std::memcmp(received.data(), request, 64));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	peer.send(receiver, 2, request, 27);
	peer.send(receiver, 2, request + 27, 27);
	peer.send(receiver, 3, request + 54, 10);
	uint8_t reply[64];
	uint32_t version;
	receiver.peekFeature(0, reply, version);
	CHECK(!std::memcmp(reply, request, 64));
	receiver.consumeFeature(0, version - 1);
	receiver.peekFeature(0, reply, version);
	CHECK(!std::memcmp(reply, request, 64));
	receiver.consumeFeature(0, version);
	receiver.peekFeature(0, reply, version);
	CHECK(reply[0] == 0x84 && reply[1] == 0);
	for (unsigned i = 0; i < 3; ++i)
		peer.send(receiver, 2, request, 27);
	peer.send(receiver, 3, request, 27);
	receiver.peekFeature(0, reply, version);
	CHECK(reply[1] == 0);
	uint8_t key_update[17] = {1};
	for (unsigned i = 0; i < 16; ++i)
		key_update[i + 1] = uint8_t(0xd0 + i);
	peer.send(receiver, 1, key_update, sizeof(key_update));
	std::memcpy(peer.key, key_update + 1, 16);
	peer.down = 0;
	receiver.service();
	drainInitial(receiver, peer);
	uint8_t commit[] = {0xb3, 0};
	storage_ok = false;
	CHECK(!receiver.feature(0, commit, 2));
	CHECK(receiver.pending(0));
	storage_ok = true;
	CHECK(receiver.feature(0, commit, 2));
	CHECK(!receiver.pending(0));
	CHECK(stored.bonds[0].used && !std::memcmp(stored.bonds[0].key, peer.key, 16));
	receiver.service();
	auto committed = peer.read(receiver, 1);
	CHECK(committed.size() == 1 && committed[0] == 5);
	for (unsigned i = 0; i < 41 * 8; ++i)
		receiver.hopTick();
	CHECK(!receiver.connected(0));
	CHECK(receiver.begin(stored, hooks));
	peer = connect(receiver, 0, true, peer.key);
	CHECK(receiver.connected(0) && !receiver.pending(0));
	std::puts("PASS: pairing, bidirectional ACK lanes, reports, fragments, key update, commit failure/recovery and reconnect");
}

static void testIsolationAndGates()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	std::array<Peer, 4> peers;
	for (unsigned i = 0; i < 4; ++i) {
		peers[i] = connect(receiver, i);
		CHECK(peers[i].slot == i);
	}
	CHECK(receiver.activePipes() == 0x3e);
	const uint8_t admission_proof[8] = {4};
	for (Peer &peer : peers)
		peer.send(receiver, 1, admission_proof, 8);
	CHECK(receiver.activePipes() == 0x3c);
	receiver.pairing(true);
	CHECK(!receiver.pairing());
	CHECK(receiver.acknowledgement(1, 0).size == 0);
	uint8_t status[] = {0xb4, 0}, reply[64];
	uint32_t version;
	for (unsigned i = 0; i < 4; ++i) {
		CHECK(receiver.feature(i, status, 2));
		receiver.peekFeature(i, reply, version);
		CHECK(reply[0] == 0xb4 && reply[2] == 3);
	}
	CHECK(!receiver.feature(4, status, 2));
	CHECK(!receiver.feature(0, status, 1));
	uint8_t invalid[] = {0x87, 63};
	CHECK(!receiver.feature(0, invalid, 2));
	sc::Packet forged{};
	forged.size = 32;
	forged.data[0] = 0x10;
	uint32_t authenticated = receiver.statistics().authenticated;
	receiver.receive(2, forged);
	CHECK(receiver.statistics().authenticated == authenticated);
	CHECK(receiver.statistics().mic_failures > 0);
	receiver.disconnect(1);
	CHECK(receiver.connected(0) && !receiver.connected(1) && receiver.connected(2));
	random_ok = false;
	CHECK(!receiver.begin(database(), hooks));
	CHECK(receiver.activePipes() == 0);
	random_ok = true;
	std::puts("PASS: four-slot isolation, malformed requests, forged MIC and RNG failure gates");
}

static void testInitialKeyChange()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	Peer peer = connect(receiver, 2);
	uint8_t update[17] = {1};
	for (unsigned i = 0; i < 16; ++i)
		update[i + 1] = uint8_t(i + 0x91);
	peer.send(receiver, 1, update, 17);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	const uint8_t empty_input[] = {0, 0};
	uint32_t duplicates = receiver.statistics().duplicates;
	peer.send(receiver, 0, empty_input, 2);
	CHECK(receiver.statistics().duplicates == duplicates + 1);
	receiver.service();
	drainInitial(receiver, peer);
	CHECK(receiver.connected(peer.slot));
	std::puts("PASS: initial counter-one key update and reset-to-one duplicate boundary");
}

static void testPairingWireAndBatchedRekey()
{
	// P:3306..3316 and the reference nRF5 ESB encoder produce these S1
	// bytes for PID 0..3. All four request an ACK, not its suppression.
	const uint8_t requested[4] = {1, 3, 5, 7};
	const uint8_t noack[4] = {0, 2, 4, 6};
	for (unsigned i = 0; i < 4; ++i) {
		CHECK(radio::wantsAck(true, requested[i]));
		CHECK(!radio::wantsAck(true, noack[i]));
		CHECK(!radio::wantsAck(false, requested[i]));
	}
	CHECK(radio::canRetune(false, false, false));
	CHECK(!radio::canRetune(true, false, false));
	CHECK(!radio::canRetune(false, true, false));
	CHECK(!radio::canRetune(false, false, true));
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	Peer peer = connect(receiver, 0, false, nullptr, false);
	uint8_t update[17] = {1};
	for (unsigned i = 0; i < 16; ++i)
		update[i + 1] = uint8_t(0x70 + i);
	sc::Packet request;
	CHECK(sc::seal(aes, peer.key, peer.iv, 1, false, 0x11, update, 17, request));
	// Coalesced RX processing can consume join and rekey before service()
	// ever serialized the initial bootstrap control04.
	receiver.receive(2, request);
	receiver.service();
	auto before = receiver.acknowledgement(2, 0);
	uint8_t plain[27];
	unsigned size;
	CHECK(sc::open(aes, sc::BOOTSTRAP_KEY, peer.iv, 1, true, before, plain, size));
	CHECK(size == 8 && plain[0] == 4 && before.data[0] == 5);
	uint32_t failures = receiver.statistics().mic_failures;
	receiver.receive(2, request);
	receiver.service();
	auto retry = receiver.acknowledgement(2, 0);
	CHECK(retry.size == before.size && !std::memcmp(retry.data, before.data, before.size));
	CHECK(receiver.statistics().mic_failures == failures);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	const uint8_t input[] = {0, 0};
	peer.send(receiver, 0, input, 2);
	auto switched = receiver.acknowledgement(2, 0);
	CHECK(sc::open(aes, peer.key, peer.iv, 1, true, switched, plain, size));
	CHECK(!sc::open(aes, sc::BOOTSTRAP_KEY, peer.iv, 1, true, switched, plain, size));
	drainInitial(receiver, peer);
	CHECK(receiver.connected(0));
	std::puts("PASS: real ACK-request S1 polarity, hop deferral, batched join/rekey and bootstrap ACK retry");
}

static std::array<uint8_t, 64> localFeature(sc::Receiver &receiver, unsigned slot,
					 uint8_t command, const std::vector<uint8_t> &args = {})
{
	CHECK(args.size() <= 62);
	uint8_t request[64] = {command, uint8_t(args.size())};
	std::memcpy(request + 2, args.data(), args.size());
	CHECK(receiver.feature(slot, request, 64));
	std::array<uint8_t, 64> reply{};
	uint32_t version;
	receiver.peekFeature(slot, reply.data(), version);
	CHECK(reply[0] == command && reply[1] >= 1);
	receiver.consumeFeature(slot, version);
	return reply;
}

static uint32_t readToken(const std::array<uint8_t, 64> &reply, unsigned offset)
{
	return uint32_t(reply[offset]) | (uint32_t(reply[offset + 1]) << 8) |
	       (uint32_t(reply[offset + 2]) << 16) | (uint32_t(reply[offset + 3]) << 24);
}

static void testFeatureIsolation()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	uint8_t wireless[] = {0x84, 1, 0x42};
	CHECK(receiver.feature(0, wireless, sizeof(wireless)));
	receiver.service();
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(wireless, wireless + 3));
	peer.send(receiver, 3, wireless, sizeof(wireless));
	// A reply may already be buffered when the console switches to diagnostics.
	CHECK(localFeature(receiver, 0, 0xb4)[2] == 3);
	CHECK(localFeature(receiver, 0, 0xa1)[1] == 34);
	CHECK(localFeature(receiver, 0, 0xf0)[3] & 4);
	CHECK(localFeature(receiver, 0, 0xf7)[1] == 62);
	CHECK(localFeature(receiver, 0, 0xf8)[4] == 0);
	uint8_t snapshot[] = {0xf1, 0};
	CHECK(receiver.feature(0, snapshot, sizeof(snapshot)));
	// An asynchronous radio reply must not overwrite a binding snapshot either.
	peer.send(receiver, 3, wireless, sizeof(wireless));
	uint8_t reply[64];
	uint32_t version;
	receiver.peekFeature(0, reply, version);
	CHECK(reply[0] == 0xf1 && reply[1] == 13 && reply[2] == 0);
	receiver.consumeFeature(0, version);
	CHECK(receiver.feature(0, wireless, sizeof(wireless)));
	receiver.peekFeature(0, reply, version);
	CHECK(!std::memcmp(reply, wireless, sizeof(wireless)));
	receiver.consumeFeature(0, version);
	CHECK(localFeature(receiver, 0, 0xb4)[2] == 3);
	// Polling an incomplete reply must not cancel deduplication of its request.
	wireless[2] = 0x43;
	CHECK(receiver.feature(0, wireless, sizeof(wireless)));
	receiver.service();
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(wireless, wireless + 3));
	receiver.peekFeature(0, reply, version);
	CHECK(reply[1] == 0);
	receiver.consumeFeature(0, version);
	CHECK(receiver.feature(0, wireless, sizeof(wireless)));
	receiver.service();
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	peer.send(receiver, 3, wireless, sizeof(wireless));
	receiver.peekFeature(0, reply, version);
	CHECK(!std::memcmp(reply, wireless, sizeof(wireless)));
	// A different argument to the same command is a new request, not a cache hit.
	wireless[2] = 0x44;
	CHECK(receiver.feature(0, wireless, sizeof(wireless)));
	receiver.peekFeature(0, reply, version);
	CHECK(reply[1] == 0);
	receiver.service();
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(wireless, wireless + 3));
	uint8_t disconnect[] = {0xb2, 0};
	CHECK(receiver.feature(0, disconnect, sizeof(disconnect)));
	receiver.peekFeature(0, reply, version);
	CHECK(reply[0] == 0xb2 && !receiver.connected(0));
	CHECK(localFeature(receiver, 0, 0xb4)[2] == 1);
	uint8_t commit[] = {0xb3, 0};
	CHECK(receiver.feature(0, commit, sizeof(commit)));
	CHECK(reports.back().bytes[4] == 1);
	std::puts("PASS: local diagnostics/snapshots isolated from asynchronous wireless replies and pending GETs");
}

static std::vector<uint8_t> tokenArgs(uint32_t token)
{
	return {uint8_t(token), uint8_t(token >> 8), uint8_t(token >> 16), uint8_t(token >> 24)};
}

static void testBootloaderFeature()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	const std::vector<uint8_t> magic = {'U', 'F', '2', '!'};
	CHECK(localFeature(receiver, 0, 0xf9)[2] == 1);
	CHECK(localFeature(receiver, 0, 0xf9, {'U', 'F', '2', '?'})[2] == 1);
	CHECK(!receiver.takeBootloaderRequest());
	auto stage = localFeature(receiver, 0, 0xf3, {0xa8, 0x12, 0x34, 0x56, 0x78, 0});
	CHECK(localFeature(receiver, 1, 0xf9, magic)[2] == 4);
	CHECK(!receiver.takeBootloaderRequest());
	CHECK(localFeature(receiver, 0, 0xf6, tokenArgs(readToken(stage, 4)))[2] == 0);
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	uint8_t reset[] = {0xf9, 4, 'U', 'F', '2', '!'}, reply[64];
	uint32_t version;
	unsigned before = saves;
	CHECK(receiver.feature(0, reset, sizeof(reset)));
	receiver.peekFeature(0, reply, version);
	CHECK(reply[0] == 0xf9 && reply[2] == 0 && reply[3] == 1);
	CHECK(!receiver.takeBootloaderRequest());
	uint32_t obsolete = version;
	CHECK(localFeature(receiver, 0, 0xf0)[2] == 3);
	receiver.consumeFeature(0, obsolete);
	CHECK(!receiver.takeBootloaderRequest());
	CHECK(receiver.feature(0, reset, sizeof(reset)));
	const uint8_t asynchronous[] = {0x87, 1, 0x42};
	peer.send(receiver, 3, asynchronous, sizeof(asynchronous));
	receiver.peekFeature(0, reply, version);
	CHECK(reply[0] == 0xf9 && reply[2] == 0);
	receiver.consumeFeature(0, version);
	CHECK(receiver.takeBootloaderRequest());
	CHECK(!receiver.takeBootloaderRequest());
	CHECK(saves == before && receiver.connected(0));
	std::puts("PASS: UF2 request magic/import/version gates, wireless isolation and acknowledged one-shot reset");
}

static void testSteamUsb()
{
	sc::Receiver receiver;
	sc::Hooks usb_hooks = {aes, randomBytes, persist, usb::input, usb::desktop};
	CHECK(receiver.begin(database(), usb_hooks));
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	usb::publish(receiver);
	auto *hid = Adafruit_USBD_HID::instances[1];
	uint8_t request[64] = {0xb4, 0}, reply[64];
	CHECK(mockFeatureSet(1, request, 64, 64));
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
	usb::process(receiver);
	usb::publish(receiver);
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64 && reply[2] == 3);
	usb::process(receiver);
	request[0] = 0x83;
	CHECK(!mockFeatureSet(1, request, 64, 64));
	usb::process(receiver);
	receiver.service();
	usb::publish(receiver);
	for (unsigned i = 0; i < 50; ++i) CHECK(!mockFeatureSet(1, request, 64, 64));
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0x83, 0}));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	std::array<uint8_t, 64> attributes{0x83, 50};
	for (unsigned i = 2; i < 52; ++i) attributes[i] = uint8_t(i);
	peer.send(receiver, 2, attributes.data(), 27);
	peer.send(receiver, 3, attributes.data() + 27, 25);
	usb::publish(receiver);
	CHECK(mockFeatureSet(1, request, 64, 64));
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(!std::memcmp(reply, attributes.data(), 64));
	// No loop service between GET completion and the next identical query.
	// The bridge must retire readiness immediately, not return the old reply.
	usb::publish(receiver); // Consumption is queued: publishing must not resurrect it.
	CHECK(!mockFeatureSet(1, request, 64, 64));
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
	usb::process(receiver);
	receiver.service();
	CHECK(!mockFeatureSet(1, request, 64, 64)); // No stale Ready between process/publish.
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0x83, 0}));
	peer.send(receiver, 2, attributes.data(), 27);
	peer.send(receiver, 3, attributes.data() + 27, 25);
	usb::publish(receiver);
	CHECK(mockFeatureSet(1, request, 64, 64));
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(!std::memcmp(reply, attributes.data(), 64));
	usb::process(receiver);
	usb::publish(receiver);
	// Serial is a separate asynchronous query; successful attributes do not
	// make an empty serial placeholder acceptable to a legacy HID client.
	request[0] = 0xae; request[1] = 1; request[2] = 1;
	CHECK(!mockFeatureSet(1, request, 64, 64));
	usb::process(receiver);
	receiver.service();
	usb::publish(receiver);
	CHECK(!mockFeatureSet(1, request, 64, 64));
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0xae, 1, 1}));
	const uint8_t serial[] = {0xae, 12, 1, 'T', 'E', 'S', 'T', '0', '0', '0', '0', '0', '1', 0};
	peer.send(receiver, 3, serial, sizeof(serial));
	usb::publish(receiver);
	CHECK(mockFeatureSet(1, request, 64, 64));
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(!std::memcmp(reply, serial, sizeof(serial)));
	usb::process(receiver);
	usb::publish(receiver);
	// Writes are accepted once; no unsolicited reply is required for settings.
	request[0] = 0x87; request[1] = 3; request[2] = 49; request[3] = 2; request[4] = 0;
	CHECK(mockFeatureSet(1, request, 64, 64));
	usb::process(receiver);
	receiver.service();
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + 5));
	// Short OUT with a stale old tail and a failed transfer cannot pair a device.
	request[0] = 0xad; request[1] = 2; request[2] = 1; request[3] = 0;
	CHECK(!mockFeatureSet(2, request, 64, 2));
	CHECK(!mockFeatureSet(2, request, 64, 64, XFER_RESULT_FAILED));
	CHECK(!mockFeatureSet(2, request, 0, 0));
	CHECK(!mockFeatureSet(2, request, 1, 1));
	usb::process(receiver);
	CHECK(!receiver.pairing());
	// Offline query completes after local dispatch and returns the real empty
	// offline response, without waiting for a nonexistent radio peer.
	request[0] = 0x83; request[1] = 0;
	CHECK(!mockFeatureSet(2, request, 64, 64));
	usb::process(receiver);
	usb::publish(receiver);
	CHECK(mockFeatureSet(2, request, 64, 64));
	CHECK(Adafruit_USBD_HID::instances[2]->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(reply[0] == 0x83 && reply[1] == 0);
	usb::process(receiver);
	request[0] = 0xfa;
	CHECK(mockFeatureSet(2, request, 64, 64));
	usb::process(receiver);
	usb::publish(receiver);
	CHECK(Adafruit_USBD_HID::instances[2]->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(reply[0] == 0xfa && reply[1] == 62 && reply[3] == radio::FIRMWARE_REVISION && reply[63] == radio::FIRMWARE_REVISION);
	CHECK(reply[16] >= 50 && reply[20] >= 3 && reply[32] >= 2);
	CHECK(reply[60] == 1);
	usb::process(receiver);
	std::puts("PASS: legacy HID SET retry/readiness, attributes -> serial -> settings, offline queries and short OUT rejection");
}

static void testHapticUsb()
{
	sc::Receiver receiver;
	sc::Hooks usb_hooks = {aes, randomBytes, persist, usb::input, usb::desktop};
	CHECK(receiver.begin(database(), usb_hooks));
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	usb::publish(receiver);
	auto *hid = Adafruit_USBD_HID::instances[1];
	uint8_t request[64] = {0x8f, 7, 0, 0x2c, 1, 0x90, 1, 2, 0}, reply[64];
	// Each SET is a new pulse, even with identical bytes and no radio reply.
	// Exercise both pad selectors, a burst before RF service, and optional GETs.
	for (unsigned pad = 0; pad < 2; ++pad) {
		request[2] = uint8_t(pad);
		for (unsigned i = 0; i < 3; ++i) {
			CHECK(mockFeatureSet(1, request, 64, 64));
			usb::process(receiver);
			usb::publish(receiver);
			CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
			CHECK(reply[0] == 0x8f && reply[1] == 0);
			usb::process(receiver);
		}
		receiver.service();
		for (unsigned i = 0; i < 3; ++i)
			CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + 9));
		CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
		// A later identical pulse must work without a GET or a fabricated reply.
		CHECK(mockFeatureSet(1, request, 64, 64));
		usb::process(receiver);
		receiver.service();
		CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + 9));
		// Even if a peer does send a write response, it cannot cache-hit a SET.
		const uint8_t response[] = {0x8f, 0};
		peer.send(receiver, 3, response, sizeof(response));
		CHECK(mockFeatureSet(1, request, 64, 64));
		usb::process(receiver);
		receiver.service();
		CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + 9));
	}
	// General settings writes have the same event semantics, including a
	// complete 64-byte command split over 27+27+10-byte radio fragments.
	request[0] = 0x87; request[1] = 62;
	for (unsigned i = 2; i < 64; ++i) request[i] = uint8_t(i);
	for (unsigned i = 0; i < 2; ++i) CHECK(mockFeatureSet(1, request, 64, 64));
	usb::process(receiver);
	receiver.service();
	for (unsigned i = 0; i < 2; ++i) {
		std::vector<uint8_t> received;
		for (unsigned part = 0; part < 3; ++part) {
			auto bytes = peer.read(receiver, part == 2 ? 3 : 2);
			received.insert(received.end(), bytes.begin(), bytes.end());
		}
		CHECK(received == std::vector<uint8_t>(request, request + 64));
	}
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	// Identical writes consume bounded queue space, not a silent dedup hit.
	request[0] = 0x8f; request[1] = 7;
	for (unsigned i = 0; i < 12; ++i) CHECK(receiver.feature(0, request, 9));
	CHECK(!receiver.feature(0, request, 9));
	CHECK(receiver.statistics().queue_full == 1);
	receiver.service();
	for (unsigned i = 0; i < 12; ++i)
		CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(request, request + 9));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	CHECK(receiver.statistics().invalid == 0 && receiver.statistics().mic_failures == 0);
	std::puts("PASS: repeated left/right haptic SETs, no-reply writes, cached write responses, settings fragments and bounded FIFO");
}

static void testUsbBackpressure()
{
	sc::Receiver receiver;
	sc::Hooks usb_hooks = {aes, randomBytes, persist, usb::input, usb::desktop};
	CHECK(receiver.begin(database(), usb_hooks));
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(receiver);
	usb::publish(receiver);
	auto *hid = Adafruit_USBD_HID::instances[1];
	uint8_t reply[64], full_write[64] = {0x87, 62};
	for (unsigned i = 2; i < 64; ++i) full_write[i] = uint8_t(i);
	const uint8_t trailing[] = {0x87, 1, 0xfe};
	auto fill = [&]() {
		for (unsigned i = 0; i < 12; ++i) {
			uint8_t body[] = {0x87, 1, uint8_t(i)};
			CHECK(receiver.feature(0, body, sizeof(body)));
		}
	};
	auto drain = [&]() {
		for (unsigned i = 0; i < 12; ++i) {
			CHECK(peer.read(receiver, 3) == std::vector<uint8_t>({0x87, 1, uint8_t(i)}));
			usb::process(receiver);
		}
	};
	// Cross ring wrap boundaries; a successful USB SET must retain all three
	// fragments even when RF has zero, then only two, free FIFO entries.
	for (unsigned round = 0; round < 3; ++round) {
		fill();
		CHECK(mockFeatureSet(1, full_write, 64, 64));
		CHECK(mockFeatureSet(1, trailing, sizeof(trailing), sizeof(trailing)));
		usb::process(receiver);
		usb::publish(receiver);
		CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
		receiver.service(); // Two entries are now serialized into ACK lanes.
		usb::process(receiver);
		usb::publish(receiver);
		CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 0);
		CHECK(receiver.statistics().queue_full == 0);
		drain();
		std::vector<uint8_t> received;
		for (unsigned part = 0; part < 3; ++part) {
			auto bytes = peer.read(receiver, part == 2 ? 3 : 2);
			received.insert(received.end(), bytes.begin(), bytes.end());
		}
		CHECK(received == std::vector<uint8_t>(full_write, full_write + 64));
		CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(trailing, trailing + sizeof(trailing)));
		CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	}
	// Original best-effort writes succeed at USB, count RF-full loss and never
	// replay a stale pulse later. They must not be confused with read queries.
	fill();
	const uint8_t commands[] = {0x8f, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6};
	for (uint8_t command : commands) {
		uint8_t body[] = {command, 0};
		CHECK(mockFeatureSet(1, body, sizeof(body), sizeof(body)));
		usb::process(receiver);
	}
	CHECK(receiver.statistics().queue_full == 0);
	mock_micros += sc::queue_policy::HAPTIC_WAIT_US;
	usb::process(receiver, 16);
	CHECK(receiver.statistics().queue_full == sizeof(commands));
	receiver.service();
	drain();
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	// Busy read SETs remain deduplicated while waiting for RF admission too.
	fill();
	const uint8_t query[] = {0x83, 0};
	for (unsigned i = 0; i < 50; ++i) CHECK(!mockFeatureSet(1, query, 2, 2));
	usb::process(receiver);
	receiver.service();
	usb::process(receiver);
	drain();
	CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(query, query + 2));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	const uint8_t response[] = {0x83, 1, 0x5a};
	peer.send(receiver, 3, response, sizeof(response));
	usb::publish(receiver);
	CHECK(mockFeatureSet(1, query, 2, 2));
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(!std::memcmp(reply, response, sizeof(response)));
	usb::process(receiver);
	// A disconnected full RF queue must not leave the USB ring stuck forever.
	fill();
	CHECK(mockFeatureSet(1, full_write, 64, 64));
	usb::process(receiver);
	receiver.disconnect(0);
	usb::process(receiver);
	usb::publish(receiver);
	CHECK(hid->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64 && reply[0] == 0 && reply[1] == 0);
	usb::process(receiver);
	CHECK(receiver.statistics().invalid == 0 && receiver.statistics().mic_failures == 0);
	std::puts("PASS: USB-to-RF backpressure, whole-command FIFO ordering, best-effort loss, busy query dedup and disconnect drain");
}

static void testUsbSessionBinding()
{
	sc::Database db = database();
	db.bonds[0].used = true;
	for (unsigned i = 0; i < 8; ++i) db.bonds[0].id[i] = uint8_t(0x30 + i);
	std::memset(db.bonds[0].key, 0x72, 16);
	sc::Receiver receiver;
	sc::Hooks usb_hooks = {aes, randomBytes, persist, usb::input, usb::desktop};
	CHECK(receiver.begin(db, usb_hooks));
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(receiver);
	usb::publish(receiver);
	auto diagnostics = [&]() {
		const uint8_t query[] = {0xfb, 0};
		CHECK(mockFeatureSet(4, query, 2, 2));
		usb::process(receiver);
		usb::publish(receiver);
		std::array<uint8_t, 64> out{};
		CHECK(Adafruit_USBD_HID::instances[4]->get(0, HID_REPORT_TYPE_FEATURE, out.data(), 64) == 64);
		CHECK(out[0] == 0xfb && out[2] == 1 && out[3] == radio::FIRMWARE_REVISION);
		usb::process(receiver);
		return out;
	};
	auto before = diagnostics();
	const uint8_t local[] = {0xb4, 0}, close[] = {0xb2, 0}, commit[] = {0xb3, 0};
	uint8_t setting[64] = {0x87, 62};
	for (unsigned i = 0; i < 16; ++i) CHECK(mockFeatureSet(2, local, 2, 2));
	CHECK(mockFeatureSet(1, close, 2, 2));
	CHECK(mockFeatureSet(1, setting, 64, 64));
	CHECK(mockFeatureSet(1, commit, 2, 2));
	usb::process(receiver, 16); // Explicit budget leaves the three target commands queued.
	Peer peer = connect(receiver, 0, true, db.bonds[0].key);
	drainInitial(receiver, peer);
	// Deliberately no publication between the actual join and executing B2:
	// USB must have captured the actual epoch, not a cached connected mask.
	usb::process(receiver);
	CHECK(receiver.connected(0) && !receiver.pending(0));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	auto after = diagnostics();
	CHECK(readToken(after, 28) == readToken(before, 28) + 3);
	CHECK(readToken(after, 8) == 0 && after[60] == 1);
	// A full RF FIFO can delay the whole USB queue across session replacement.
	const uint8_t pulse[] = {0x8f, 0};
	for (unsigned i = 0; i < 12; ++i) CHECK(receiver.feature(0, pulse, 2));
	CHECK(mockFeatureSet(1, setting, 64, 64));
	CHECK(mockFeatureSet(1, close, 2, 2));
	usb::process(receiver);
	receiver.disconnect(0);
	peer = connect(receiver, 0, true, db.bonds[0].key);
	drainInitial(receiver, peer);
	usb::process(receiver);
	CHECK(receiver.connected(0));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	after = diagnostics();
	CHECK(readToken(after, 28) == readToken(before, 28) + 5);
	CHECK(readToken(after, 8) == 1);
	// Admission occurs at DATA, not the later void ACK callback. A session
	// can turn over while EP0 is in flight, before any Command exists yet.
	CHECK(mockFeatureData(1, close, 2, 2));
	receiver.disconnect(0);
	peer = connect(receiver, 0, true, db.bonds[0].key);
	drainInitial(receiver, peer);
	CHECK(mockFeatureAck());
	usb::process(receiver);
	CHECK(receiver.connected(0));
	after = diagnostics();
	CHECK(readToken(after, 28) == readToken(before, 28) + 6 && readToken(after, 8) == 2);
	CHECK(mockFeatureSet(1, close, 2, 2)); // Current-session B2 still works.
	usb::process(receiver);
	CHECK(!receiver.connected(0));
	after = diagnostics();
	CHECK(readToken(after, 8) == 3 && after[32] == uint8_t(sc::DisconnectReason::Host));
	std::puts("PASS: deferred offline B2/B3/settings and blocked old-session writes cannot target a newly restored session; FB reasons/stale counts");
}

static void testEntropyAndHapticsUsb()
{
	sc::Receiver receiver;
	sc::Hooks usb_hooks = {aes, randomBytes, persist, usb::input, usb::desktop};
	CHECK(receiver.begin(database(), usb_hooks));
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	tud_event_hook_cb(0, DCD_EVENT_BUS_RESET, true);
	usb::process(receiver);
	uint8_t entropy[64] = {0x96, 16};
	uint8_t pulse[64] = {0x8f, 7, 0, 0x2c, 1, 0x90, 1, 2, 0};
	unsigned before_saves = saves, before_random = random_byte;
	uint8_t challenge[8];
	std::memcpy(challenge, receiver.challenge(), sizeof(challenge));
	// Repeat the four-interface startup traffic while one connected controller
	// continues sending inputs and receiving distinct left/right haptic writes.
	for (unsigned round = 0; round < 40; ++round) {
		for (unsigned i = 2; i < 18; ++i) entropy[i] = uint8_t(round + i);
		for (unsigned itf = 1; itf <= 4; ++itf)
			CHECK(mockFeatureSet(itf, entropy, 64, 64));
		for (unsigned pad = 0; pad < 2; ++pad) {
			pulse[2] = uint8_t(pad);
			CHECK(mockFeatureSet(1, pulse, 64, 64));
		}
		usb::process(receiver);
		usb::process(receiver); // Four-slot entropy plus two pulse records.
		receiver.service();
		for (unsigned pad = 0; pad < 2; ++pad) {
			pulse[2] = uint8_t(pad);
			CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(pulse, pulse + 9));
		}
		CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
		const uint8_t input_body[] = {1, 0, uint8_t(round)};
		peer.send(receiver, 0, input_body, sizeof(input_body));
		CHECK(receiver.connected(0));
	}
	CHECK(receiver.statistics().queue_full == 0 && receiver.statistics().mic_failures == 0);
	CHECK(saves == before_saves && random_byte == before_random);
	CHECK(!std::memcmp(receiver.challenge(), challenge, sizeof(challenge)));
	CHECK(!receiver.takeBootloaderRequest() && !receiver.takeIdentityChange());
	// Full RF FIFO does not turn local entropy into a busy wireless command.
	for (unsigned i = 0; i < 12; ++i) CHECK(receiver.feature(0, pulse, 64));
	for (unsigned itf = 1; itf <= 4; ++itf) CHECK(mockFeatureSet(itf, entropy, 64, 64));
	usb::process(receiver);
	usb::publish(receiver);
	uint8_t reply[64];
	CHECK(Adafruit_USBD_HID::instances[1]->get(0, HID_REPORT_TYPE_FEATURE, reply, 64) == 64);
	CHECK(reply[0] == 0x96 && reply[1] == 0);
	usb::process(receiver);
	receiver.service();
	for (unsigned i = 0; i < 12; ++i)
		CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(pulse, pulse + 9));
	CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	// Reject truncated frames even after a full entropy packet populated EP0.
	CHECK(!mockFeatureSet(1, entropy, 64, 17));
	CHECK(!mockFeatureSet(1, entropy, 64, 18, XFER_RESULT_FAILED));
	CHECK(mockFeatureSet(1, entropy, 18, 18));
	usb::process(receiver);
	CHECK(receiver.connected(0) && receiver.statistics().queue_full == 0);
	std::puts("PASS: four-slot PRNG entropy startup with continuous encrypted input/haptics, full RF FIFO, no Flash/rekey and short OUT rejection");
}

static void testUsbInputLifecycle()
{
	sc::Receiver receiver;
	sc::Hooks usb_hooks = {aes, randomBytes, persist, usb::input, usb::desktop};
	CHECK(receiver.begin(database(), usb_hooks));
	Peer peer = connect(receiver);
	drainInitial(receiver, peer);
	auto &desktop_hid = *Adafruit_USBD_HID::instances[0];
	auto &slot = *Adafruit_USBD_HID::instances[1];
	const uint8_t sample[] = {0x21, 0x60, 0x81, 4, 100, 0, 1}; // button, key, dx, mouse button
	const uint8_t empty[] = {0, 0};
	const uint8_t disable[] = {0xb1, 2, 0, 0}, enable[] = {0xb1, 2, 0, 1};
	for (unsigned event : {DCD_EVENT_BUS_RESET, DCD_EVENT_UNPLUGGED, DCD_EVENT_RESUME}) {
		for (unsigned i = 0; i < 3; ++i) peer.send(receiver, 0, sample, sizeof(sample));
		slot.sent.clear();
		desktop_hid.sent.clear();
		// RESUME retains feature commands; reset/unplug cancels only USB work.
		const uint8_t write[] = {0x87, 1, 0x42};
		CHECK(mockFeatureSet(1, write, sizeof(write), sizeof(write)));
		unsigned before_saves = saves;
		tud_event_hook_cb(0, event, true);
		usb::process(receiver);
		usb::publish(receiver);
		for (unsigned i = 0; i < 8; ++i) usb::pump();
		CHECK(slot.sent.size() == 1 && slot.sent[0][2] == 1);
		for (unsigned i = 8; i < 64; ++i) CHECK(slot.sent[0][i] == 0);
		CHECK(desktop_hid.sent.size() == 2);
		for (const auto &report : desktop_hid.sent)
			for (size_t i = 1; i < report.size(); ++i) CHECK(report[i] == 0);
		CHECK(receiver.connected(0) && saves == before_saves);
		// Same authenticated peer/counter still works, without old state tails.
		peer.send(receiver, 0, empty, sizeof(empty));
		usb::pump();
		for (unsigned i = 8; i < 64; ++i) CHECK(slot.sent.back()[i] == 0);
		receiver.service();
		if (event == DCD_EVENT_RESUME)
			CHECK(peer.read(receiver, 3) == std::vector<uint8_t>(write, write + sizeof(write)));
		CHECK(receiver.acknowledgement(2, unsigned(peer.down & 1)).size == 1);
	}
	// Off and on can arrive in the same loop batch. Old movement must be
	// cleared at the off edge, not replayed because the final state is on.
	for (unsigned i = 0; i < 3; ++i) peer.send(receiver, 0, sample, sizeof(sample));
	desktop_hid.sent.clear();
	CHECK(mockFeatureSet(1, disable, sizeof(disable), sizeof(disable)));
	CHECK(mockFeatureSet(1, enable, sizeof(enable), sizeof(enable)));
	usb::process(receiver);
	for (unsigned i = 0; i < 8; ++i) usb::pump();
	CHECK(receiver.desktopEnabled() && desktop_hid.sent.size() == 2);
	CHECK(desktop_hid.sent[0] == std::vector<uint8_t>({1, 0, 0, 4, 0, 0, 0, 0, 0}));
	CHECK(desktop_hid.sent[1] == std::vector<uint8_t>({2, 1, 0, 0, 0}));
	std::puts("PASS: USB reset/unplug/resume input release, preserved RF sessions and batched B1 off/on motion discard");
}

static void testMigration()
{
	sc::Database db = database();
	db.bonds[19].used = true;
	std::memset(db.bonds[19].id, 0x51, 8);
	std::memset(db.bonds[19].key, 0x72, 16);
	sc::Receiver receiver;
	CHECK(receiver.begin(db, hooks));
	for (unsigned slot = 0; slot < 4; ++slot) {
		auto diag = localFeature(receiver, slot, 0xf0);
		CHECK(diag[2] == 3 && diag[33] == slot && diag[34] == 4);
		CHECK(!std::memcmp(diag.data() + 59, db.address, 5));
	}
	auto snapshot = localFeature(receiver, 0, 0xf1);
	CHECK(snapshot[2] == 0 && snapshot[3] == 1 && snapshot[13] == 1 && snapshot[14] == 20);
	uint32_t token = readToken(snapshot, 4);
	auto record_args = tokenArgs(token);
	record_args.push_back(19);
	auto record = localFeature(receiver, 1, 0xf2, record_args);
	CHECK(record[2] == 0 && record[3] == 19 && record[4] == 1);
	CHECK(!std::memcmp(record.data() + 5, db.bonds[19].id, 8));
	CHECK(!std::memcmp(record.data() + 13, db.bonds[19].key, 16));
	CHECK(readToken(record, 29) == token);
	std::vector<uint8_t> legacy_write(24, 0x63);
	uint8_t request[26] = {0xb0, 24};
	std::memcpy(request + 2, legacy_write.data(), 24);
	CHECK(receiver.feature(0, request, 26));
	record = localFeature(receiver, 0, 0xf2, record_args);
	CHECK(!std::memcmp(record.data() + 13, db.bonds[19].key, 16));
	record_args[4] = 0;
	record = localFeature(receiver, 0, 0xf2, record_args);
	CHECK(record[2] == 0 && record[4] == 0);
	for (unsigned i = 5; i < 29; ++i) CHECK(record[i] == 0);
	CHECK(localFeature(receiver, 0, 0xf6, tokenArgs(token))[2] == 0);
	CHECK(localFeature(receiver, 0, 0xf2, record_args)[2] == 2);
	receiver.pairing(true);
	unsigned before = saves;
	std::vector<uint8_t> begin_args = {0xa8, 0x12, 0x34, 0x56, 0x78, 2};
	auto stage = localFeature(receiver, 0, 0xf3, begin_args);
	CHECK(stage[2] == 0 && stage[13] == 2);
	token = readToken(stage, 4);
	CHECK(receiver.activePipes() == 0 && !receiver.pairing());
	CHECK(!std::memcmp(receiver.database().address, db.address, 5));
	CHECK(localFeature(receiver, 0, 0xf1)[2] == 4);
	CHECK(localFeature(receiver, 0, 0xf5, tokenArgs(token))[2] == 5);
	auto write_args = tokenArgs(token);
	write_args.push_back(0);
	write_args.insert(write_args.end(), 8, 0x31);
	write_args.insert(write_args.end(), 16, 0x42);
	CHECK(localFeature(receiver, 0, 0xf4, write_args)[2] == 0);
	CHECK(localFeature(receiver, 0, 0xf4, write_args)[2] == 0);
	write_args[4] = 1;
	CHECK(localFeature(receiver, 0, 0xf4, write_args)[2] == 7);
	write_args[5] = 0x32;
	CHECK(localFeature(receiver, 0, 0xf4, write_args)[2] == 0);
	storage_ok = false;
	CHECK(localFeature(receiver, 0, 0xf5, tokenArgs(token))[2] == 6);
	CHECK(!std::memcmp(receiver.database().address, db.address, 5));
	storage_ok = true;
	CHECK(localFeature(receiver, 0, 0xf5, tokenArgs(token))[2] == 0);
	CHECK(saves == before + 2);
	CHECK(receiver.takeIdentityChange() && !receiver.takeIdentityChange());
	CHECK(!std::memcmp(receiver.database().address, begin_args.data(), 5));
	CHECK(receiver.database().bonds[0].used && receiver.database().bonds[1].used);
	CHECK(!receiver.database().bonds[19].used);
	CHECK(localFeature(receiver, 0, 0xf5, tokenArgs(token))[2] == 0);
	CHECK(saves == before + 2);
	stage = localFeature(receiver, 0, 0xf3, begin_args);
	token = readToken(stage, 4);
	CHECK(localFeature(receiver, 0, 0xf6, tokenArgs(token))[2] == 0);
	CHECK(receiver.database().bonds[0].used);
	receiver.pairing(true);
	stage = localFeature(receiver, 0, 0xf3, begin_args);
	token = readToken(stage, 4);
	for (unsigned i = 0; i < 20000; ++i) receiver.hopTick();
	CHECK(receiver.pairing() && receiver.activePipes() == 3);
	CHECK(localFeature(receiver, 0, 0xf5, tokenArgs(token))[2] == 2);
	Peer peer = connect(receiver, 3);
	CHECK(localFeature(receiver, 0, 0xf3, begin_args)[2] == 3);
	receiver.disconnect(peer.slot);
	begin_args[0] |= 1;
	CHECK(localFeature(receiver, 0, 0xf3, begin_args)[2] == 1);
	CHECK(localFeature(receiver, 0, 0xf3, {0xa8})[2] == 1);
	std::puts("PASS: identity-aware snapshots, atomic binding migration, stale tokens, incomplete writes, cancel and timeout");
}

static void testPersistedReconnectAndCommitOrdering()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	Peer peer = connect(receiver, 0, false, nullptr, false);
	unsigned before = saves;
	uint8_t commit[] = {0xb3, 0};
	CHECK(receiver.feature(0, commit, 2));
	receiver.service();
	CHECK(saves == before && receiver.pending(0));
	auto status = localFeature(receiver, 0, 0xf8);
	CHECK(status[4] == 0 && (status[61] & 1) && (status[62] & 1));
	uint8_t update[17] = {1};
	for (unsigned i = 0; i < 16; ++i) update[i + 1] = uint8_t(0xb0 + i);
	peer.send(receiver, 1, update, 17);
	receiver.service();
	CHECK(saves == before && receiver.pending(0));
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	const uint8_t sample[] = {0, 0};
	peer.send(receiver, 0, sample, 2);
	receiver.service();
	CHECK(saves == before + 1 && !receiver.pending(0));
	CHECK(!std::memcmp(stored.bonds[0].key, peer.key, 16));
	CHECK(std::memcmp(stored.bonds[0].key, sc::BOOTSTRAP_KEY, 16) != 0);
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[4] == 1 && status[5] == 0 && status[60] == 0 && status[62] == 0);
	auto info = peer.read(receiver, 1);
	CHECK(info.size() == 8 && info[0] == 4);
	auto count = peer.read(receiver, 1);
	CHECK(count.size() == 2 && count[0] == 3);
	auto confirmed = peer.read(receiver, 1);
	CHECK(confirmed.size() == 1 && confirmed[0] == 5);
	uint8_t encoded[sc::STORE_SIZE];
	CHECK(sc::encodeDatabase(stored, 9, encoded));
	sc::Database loaded{};
	uint32_t generation;
	CHECK(sc::decodeDatabase(encoded, loaded, generation) && generation == 9);
	receiver.disconnect(0);
	// The real failure report ends with H=2 repeats; do not require the
	// synthetic peer to prime an H=0 cache before every restore attempt.
	peer = connect(receiver, 0, true, loaded.bonds[0].key, true, false);
	CHECK(receiver.connected(0) && !receiver.pending(0));
	drainInitial(receiver, peer);
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[7] == 9 && readToken(status, 20) == 1 && readToken(status, 56) == 1);
	for (unsigned cycle = 0; cycle < 3; ++cycle) {
		CHECK(receiver.begin(loaded, hooks));
		peer = connect(receiver, 0, true, loaded.bonds[0].key, true, false);
		drainInitial(receiver, peer);
		CHECK(receiver.connected(0) && !receiver.pending(0));
		receiver.disconnect(0);
	}
	CHECK(receiver.begin(loaded, hooks));
	sc::Packet lookup{};
	lookup.size = 9;
	std::memcpy(lookup.data + 1, peer.id, 8);
	receiver.receive(1, lookup);
	lookup.data[1] ^= 0x7f;
	receiver.receive(1, lookup);
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[6] == 1 && readToken(status, 8) == 1 && readToken(status, 12) == 1);
	peer = connect(receiver, 0, true, loaded.bonds[0].key, true, false);
	CHECK(receiver.connected(0));
	CHECK(receiver.begin(loaded, hooks));
	auto ack = receiver.acknowledgement(1, 0);
	uint8_t wrong_id[17];
	std::memcpy(wrong_id, loaded.address, 5);
	std::memset(wrong_id + 5, 0xee, 12);
	sc::Packet forged;
	CHECK(sc::seal(aes, loaded.bonds[0].key, ack.data + 1, 0, false, 2,
		      wrong_id, 17, forged));
	receiver.receive(1, forged);
	CHECK(!receiver.connected(0));
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[7] == 7);
	CHECK(receiver.begin(database(), hooks));
	receiver.receive(1, forged);
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[4] == 0 && status[7] == 4 && readToken(status, 16) == 1);
	std::puts("PASS: deferred final-key commit, persisted H=2 restore without lookup, restart cycles and bound-ID authentication");
}

static radio::ResponseSnapshot snapshot(const sc::Receiver &receiver)
{
	radio::ResponseSnapshot result{};
	for (unsigned pipe = 0; pipe < 6; ++pipe)
		for (unsigned parity = 0; parity < 2; ++parity)
			result.acknowledgements[pipe][parity] = receiver.acknowledgement(pipe, parity);
	for (unsigned i = 0; i < 4; ++i) result.joins[i] = receiver.joinReplay(i);
	result.mask = receiver.activePipes();
	return result;
}

static sc::Database multiDatabase()
{
	sc::Database db = database();
	for (unsigned serial = 0; serial < 4; ++serial) {
		db.bonds[serial].used = true;
		for (unsigned i = 0; i < 8; ++i) db.bonds[serial].id[i] = uint8_t(0x30 + i + serial * 8);
		for (unsigned i = 0; i < 16; ++i) db.bonds[serial].key[i] = uint8_t(0x90 + i + serial * 16);
	}
	return db;
}

static int drainControls(sc::Receiver &receiver, Peer &peer)
{
	int latest_count = -1;
	for (unsigned i = 0; i < 16; ++i) {
		receiver.service();
		const uint8_t phase[] = {4};
		peer.send(receiver, 1, phase, sizeof(phase));
		if (receiver.acknowledgement(peer.slot + 2, unsigned(peer.down & 1)).size == 1)
			return latest_count;
		auto message = peer.read(receiver, 1);
		if (message.size() == 2 && message[0] == 3) latest_count = message[1];
	}
	CHECK(false);
	return latest_count;
}

static void testJoinReplayAndConnectionMatrix()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	Peer first = connect(receiver, 0, false, nullptr, false);
	sc::JoinReplay accepted = receiver.joinReplay(first.slot);
	CHECK(accepted.active && accepted.acknowledgement.size == 22);
	uint8_t current_challenge[8];
	std::memcpy(current_challenge, receiver.challenge(), 8);
	auto responses = snapshot(receiver);
	const auto &replayed = responses.select(1, 0, accepted.request.data, accepted.request.size);
	CHECK(!std::memcmp(replayed.data, accepted.acknowledgement.data, replayed.size));
	CHECK(std::memcmp(replayed.data + 1, current_challenge, 8) != 0);
	uint32_t duplicates = receiver.statistics().duplicates;
	receiver.receive(1, accepted.request);
	CHECK(receiver.statistics().duplicates == duplicates + 1);
	CHECK(receiver.connected(first.slot) && !receiver.connected(1));
	CHECK(!std::memcmp(receiver.challenge(), current_challenge, 8));
	sc::Packet altered = accepted.request;
	altered.data[altered.size - 1] ^= 1;
	const auto &normal = responses.select(1, 0, altered.data, altered.size);
	CHECK(!std::memcmp(normal.data + 1, current_challenge, 8));
	const uint8_t proof[8] = {4};
	first.send(receiver, 1, proof, 8);
	CHECK(!receiver.joinReplay(first.slot).active);
	responses = snapshot(receiver);
	CHECK(!std::memcmp(responses.select(1, 0, accepted.request.data, accepted.request.size).data + 1, current_challenge, 8));
	receiver.disconnect(first.slot);
	CHECK(!receiver.joinReplay(first.slot).active);
	CHECK(receiver.begin(multiDatabase(), hooks));
	sc::Database db = receiver.database();
	std::array<Peer, 4> peers;
	for (unsigned serial = 0; serial < 4; ++serial) {
		peers[serial] = connect(receiver, serial, true, db.bonds[serial].key, true, false);
		if (serial < 3) CHECK(drainControls(receiver, peers[serial]) == int(serial + 1));
	}
	CHECK(receiver.activePipes() == 0x3e);
	accepted = receiver.joinReplay(peers[3].slot);
	responses = snapshot(receiver);
	CHECK(responses.select(1, 0, accepted.request.data, accepted.request.size).data[9] % 8 == 5);
	receiver.receive(1, accepted.request);
	for (unsigned i = 0; i < 4; ++i) CHECK(receiver.connected(i));
	CHECK(drainControls(receiver, peers[3]) == 4);
	for (unsigned i = 0; i < 3; ++i) CHECK(drainControls(receiver, peers[i]) == 4);
	CHECK(receiver.activePipes() == 0x3c);
	sc::Packet full_join;
	uint8_t body[17];
	std::memcpy(body, db.address, 5);
	std::memcpy(body + 5, peers[0].id, 8);
	std::memset(body + 13, 0x87, 4);
	CHECK(sc::seal(aes, peers[0].key, receiver.challenge(), 0, false, 2, body, 17, full_join));
	receiver.receive(1, full_join);
	CHECK(localFeature(receiver, 0, 0xf8)[7] == 11);
	for (unsigned i = 0; i < 328; ++i) {
		receiver.hopTick();
		if ((i & 7) == 0) {
			for (unsigned j = 1; j < 4; ++j) peers[j].send(receiver, 1, proof, 8);
		}
	}
	CHECK(!receiver.connected(0));
	for (unsigned i = 1; i < 4; ++i) CHECK(receiver.connected(i));
	receiver.service();
	for (unsigned i = 1; i < 4; ++i) CHECK(drainControls(receiver, peers[i]) == 3);
	peers[0] = connect(receiver, 0, true, db.bonds[0].key, true, false);
	CHECK(peers[0].slot == 0);
	CHECK(drainControls(receiver, peers[0]) == 4);
	for (unsigned i = 1; i < 4; ++i) CHECK(drainControls(receiver, peers[i]) == 4);
	// Rapid same-ID restart with a free slot must honor the advertised new
	// pipe, release only the old session, and keep its persisted identity.
	receiver.disconnect(2);
	Peer restarted = connect(receiver, 0, true, db.bonds[0].key, true, false);
	CHECK(restarted.slot == 2 && !receiver.connected(0) && receiver.connected(1));
	CHECK(receiver.connected(2) && receiver.connected(3));
	CHECK(drainControls(receiver, restarted) == 3);
	for (unsigned i : {1u, 3u}) CHECK(drainControls(receiver, peers[i]) == 3);
	std::puts("PASS: lost join ACK replay, four-slot grace/full admission, count broadcasts, selective timeout and rapid restart");
}

static void testChallengeAndWatchdogTimeline()
{
	sc::Database db = multiDatabase();
	sc::Receiver receiver;
	CHECK(receiver.begin(db, hooks));
	// Two controllers can prepare joins against the same advertised epoch.
	// Only one may consume that pipe; the other must retry the new challenge.
	uint8_t shared[8];
	std::memcpy(shared, receiver.challenge(), 8);
	std::array<sc::Packet, 2> simultaneous;
	for (unsigned i = 0; i < 2; ++i) {
		uint8_t join_body[17];
		std::memcpy(join_body, db.address, 5);
		std::memcpy(join_body + 5, db.bonds[i].id, 8);
		std::memset(join_body + 13, int(0x81 + i), 4);
		CHECK(sc::seal(aes, db.bonds[i].key, shared, 0, false, 2, join_body, 17, simultaneous[i]));
	}
	receiver.receive(1, simultaneous[0]);
	receiver.receive(1, simultaneous[1]);
	CHECK(receiver.connected(0) && !receiver.connected(1));
	Peer synchronized = connect(receiver, 1, true, db.bonds[1].key, true, false);
	CHECK(synchronized.slot == 1 && receiver.connected(0) && receiver.connected(1));
	CHECK(receiver.begin(db, hooks));
	Peer peer = connect(receiver, 0, true, db.bonds[0].key, true, false);
	drainControls(receiver, peer);
	Peer other = connect(receiver, 1, true, db.bonds[1].key, true, false);
	drainControls(receiver, other);
	const auto old_challenge = receiver.acknowledgement(1, 0);
	uint8_t body[17];
	std::memcpy(body, db.address, 5);
	std::memcpy(body + 5, db.bonds[2].id, 8);
	std::memset(body + 13, 0x48, 4);
	sc::Packet stale;
	CHECK(sc::seal(aes, db.bonds[2].key, old_challenge.data + 1, 0, false, 2, body, 17, stale));
	receiver.disconnect(other.slot);
	receiver.receive(1, stale);
	CHECK(!receiver.connected(1) && receiver.connected(0));
	CHECK(localFeature(receiver, 0, 0xf8)[7] == 5);
	Peer fresh = connect(receiver, 2, true, db.bonds[2].key, true, false);
	CHECK(receiver.connected(fresh.slot));
	CHECK(receiver.begin(db, hooks));
	peer = connect(receiver, 0, true, db.bonds[0].key, true, false);
	drainControls(receiver, peer);
	const uint8_t sample[] = {0, 0};
	// Deliver queued packets at their recorded hop epochs before advancing
	// idle wall time. Fresh RX must not be charged all historical timer ticks.
	for (uint32_t epoch : {160u, 320u, 480u, 640u}) {
		receiver.advanceToHop(epoch);
		peer.send(receiver, 0, sample, 2);
		CHECK(receiver.connected(0));
	}
	receiver.advanceToHop(799);
	CHECK(receiver.connected(0));
	receiver.advanceToHop(640 + 328);
	CHECK(!receiver.connected(0));
	std::puts("PASS: stale challenge rejection, fresh restore and chronological RX/watchdog replay");
}

static void testDeferredFailureAndLastBond()
{
	sc::Database db = database();
	const sc::Database seeds = multiDatabase();
	db.bonds[0] = seeds.bonds[0];
	db.bonds[19] = seeds.bonds[3];
	sc::Receiver receiver;
	CHECK(receiver.begin(db, hooks));
	sc::Packet lookup{};
	lookup.size = 9;
	std::memcpy(lookup.data + 1, db.bonds[0].id, 8);
	receiver.receive(1, lookup);
	Peer peer = connect(receiver, 3, true, db.bonds[19].key, true, false);
	CHECK(receiver.connected(peer.slot));
	auto status = localFeature(receiver, 0, 0xf8);
	CHECK(readToken(status, 8) == 1 && readToken(status, 56) == 1);
	receiver.disconnect(peer.slot);
	uint8_t body[17];
	std::memcpy(body, db.address, 5);
	body[1] ^= 0x40;
	std::memcpy(body + 5, db.bonds[19].id, 8);
	std::memset(body + 13, 0x39, 4);
	sc::Packet wrong_address;
	CHECK(sc::seal(aes, db.bonds[19].key, receiver.challenge(), 0, false, 2, body, 17, wrong_address));
	receiver.receive(1, wrong_address);
	CHECK(!receiver.connected(0) && localFeature(receiver, 0, 0xf8)[7] == 6);
	std::memcpy(body, db.address, 5);
	CHECK(sc::seal(aes, sc::BOOTSTRAP_KEY, receiver.challenge(), 0, false, 2, body, 17, wrong_address));
	receiver.receive(1, wrong_address);
	CHECK(!receiver.connected(0) && localFeature(receiver, 0, 0xf8)[7] == 5);
	CHECK(receiver.begin(database(), hooks));
	peer = connect(receiver);
	uint8_t commit[] = {0xb3, 0}, update[17] = {1};
	CHECK(receiver.feature(0, commit, 2));
	for (unsigned i = 0; i < 16; ++i) update[i + 1] = uint8_t(0xf0 - i);
	peer.send(receiver, 1, update, 17);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	const uint8_t input[] = {0, 0};
	peer.send(receiver, 0, input, 2);
	unsigned before = saves;
	storage_ok = false;
	receiver.service();
	CHECK(saves == before + 1 && receiver.pending(0));
	receiver.service();
	CHECK(saves == before + 1);
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[62] == 0 && readToken(status, 48) == 1);
	storage_ok = true;
	CHECK(receiver.feature(0, commit, 2));
	CHECK(saves == before + 2 && !receiver.pending(0));
	CHECK(!std::memcmp(stored.bonds[0].key, peer.key, 16));
	CHECK(receiver.begin(database(), hooks));
	peer = connect(receiver);
	before = saves;
	CHECK(receiver.feature(0, commit, 2));
	receiver.disconnect(peer.slot);
	receiver.service();
	CHECK(saves == before);
	status = localFeature(receiver, 0, 0xf8);
	CHECK(status[4] == 0 && readToken(status, 52) == 1);
	std::puts("PASS: wrong cached selector, last bond index, receiver/key mismatch, deferred Flash failure and unsaved disconnect");
}

static void testStandalonePairingPersistence()
{
	sc::Receiver receiver;
	CHECK(receiver.begin(database(), hooks));
	unsigned before = saves;
	Peer peer = connect(receiver, 0, false, nullptr, false, true, true);
	receiver.service();
	CHECK(saves == before && receiver.pending(0));
	uint8_t update[17] = {1};
	for (unsigned i = 0; i < 16; ++i) update[i + 1] = uint8_t(0x43 + i);
	peer.send(receiver, 1, update, 17);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	const uint8_t sample[] = {0, 0};
	peer.send(receiver, 0, sample, 2);
	receiver.service();
	CHECK(saves == before + 1 && !receiver.pending(0));
	CHECK(!std::memcmp(stored.bonds[0].key, peer.key, 16));
	auto info = peer.read(receiver, 1);
	CHECK(info.size() == 8 && info[0] == 4);
	auto count = peer.read(receiver, 1);
	CHECK(count.size() == 2 && count[0] == 3);
	auto commit = peer.read(receiver, 1);
	CHECK(commit.size() == 1 && commit[0] == 5);
	sc::Database saved = stored;
	CHECK(receiver.begin(saved, hooks));
	peer = connect(receiver, 0, true, saved.bonds[0].key, true, false);
	drainControls(receiver, peer);
	CHECK(receiver.connected(0) && !receiver.pending(0));
	CHECK(saves == before + 1);
	// Host-controlled AD can deliberately override the local auto-save mode.
	CHECK(receiver.begin(database(), hooks));
	receiver.pairing(true, true);
	uint8_t host_pair[] = {0xad, 2, 1, 0};
	CHECK(receiver.feature(0, host_pair, 4));
	peer = connect(receiver, 0, false, nullptr, false, true, false);
	CHECK(localFeature(receiver, 0, 0xf8)[62] == 0);
	peer.send(receiver, 1, update, 17);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	peer.send(receiver, 0, sample, 2);
	receiver.service();
	CHECK(receiver.pending(0) && saves == before + 1);
	std::puts("PASS: standalone/button auto-save of final key, no-host restart and explicit host commit mode");
}

static void testFullBondDatabase()
{
	sc::Database full = database();
	for (unsigned i = 0; i < sc::BOND_COUNT; ++i) {
		full.bonds[i].used = true;
		std::memset(full.bonds[i].id, int(0x80 + i), 8);
		std::memset(full.bonds[i].key, int(0x20 + i), 16);
	}
	sc::Receiver receiver;
	CHECK(receiver.begin(full, hooks));
	unsigned before = saves;
	Peer peer = connect(receiver, 0, false, nullptr, false, true, true);
	uint8_t update[17] = {1};
	for (unsigned i = 0; i < 16; ++i) update[i + 1] = uint8_t(0x53 + i);
	peer.send(receiver, 1, update, 17);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	const uint8_t sample[] = {0, 0};
	peer.send(receiver, 0, sample, 2);
	receiver.service();
	CHECK(saves == before && receiver.pending(0));
	auto report = localFeature(receiver, 0, 0xf8);
	CHECK(report[4] == 20 && readToken(report, 48) == 1 && report[62] == 0);
	for (unsigned i = 0; i < sc::BOND_COUNT; ++i)
		CHECK(!std::memcmp(full.bonds[i].key, receiver.database().bonds[i].key, 16));
	// A full store can still re-pair an existing identity: replace only that
	// identity's key, retaining the other nineteen bindings.
	for (unsigned i = 0; i < 8; ++i) full.bonds[19].id[i] = uint8_t(0x30 + i);
	CHECK(receiver.begin(full, hooks));
	peer = connect(receiver, 0, false, nullptr, false, true, true);
	peer.send(receiver, 1, update, 17);
	std::memcpy(peer.key, update + 1, 16);
	peer.up = peer.down = 0;
	peer.send(receiver, 0, sample, 2);
	receiver.service();
	CHECK(saves == before + 1 && !receiver.pending(0));
	CHECK(!std::memcmp(stored.bonds[19].key, peer.key, 16));
	for (unsigned i = 0; i < 19; ++i)
		CHECK(!std::memcmp(full.bonds[i].key, stored.bonds[i].key, 16));
	sc::Database saved = stored;
	CHECK(receiver.begin(saved, hooks));
	peer = connect(receiver, 0, true, saved.bonds[19].key, true, false);
	CHECK(receiver.connected(0) && !receiver.pending(0));
	std::puts("PASS: full twenty-record bond store preserves old keys and never reports a false commit");
}

int main(int argc, char **argv)
{
	CHECK(argc == 4);
	testCrypto(argv[1]);
	testHops(argv[2]);
	testStorage();
	testLink();
	testIsolationAndGates();
	testInitialKeyChange();
	testPairingWireAndBatchedRekey();
	testMigration();
	testPersistedReconnectAndCommitOrdering();
	testJoinReplayAndConnectionMatrix();
	testChallengeAndWatchdogTimeline();
	testDeferredFailureAndLastBond();
	testStandalonePairingPersistence();
	testFullBondDatabase();
	testFeatureIsolation();
	testBootloaderFeature();
	testUsb();
	testUsbControlEdges();
	testSteamUsb();
	testHapticUsb();
	testUsbBackpressure();
	testUsbSessionBinding();
	testEntropyAndHapticsUsb();
	testUsbInputLifecycle();
	testProtocolCompatibility();
	testRadioRetention();
	testDesktopUsb();
	testQueueMetrics();
	testQueueUsb(argv[3]);
	std::printf("PASS: %u assertions; synthetic tests, not hardware interoperability\n", assertions);
}
