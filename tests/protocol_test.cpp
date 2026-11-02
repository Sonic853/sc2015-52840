#include "protocol.h"
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
		    const uint8_t *saved_key = nullptr)
{
	Peer peer;
	for (unsigned i = 0; i < 8; ++i)
		peer.id[i] = uint8_t(0x30 + i + serial * 8);
	std::memcpy(peer.key, restore ? saved_key : sc::BOOTSTRAP_KEY, 16);
	if (restore) {
		sc::Packet lookup{};
		lookup.size = 9;
		std::memcpy(lookup.data + 1, peer.id, 8);
		receiver.receive(1, lookup);
	} else {
		receiver.pairing(true);
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
	request[0] = 0x87; request[1] = 62;
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
	CHECK(reply[0] == 0x87 && reply[1] == 0);
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

static std::vector<uint8_t> tokenArgs(uint32_t token)
{
	return {uint8_t(token), uint8_t(token >> 8), uint8_t(token >> 16), uint8_t(token >> 24)};
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
		CHECK(diag[2] == 2 && diag[33] == slot && diag[34] == 4);
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

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	testCrypto(argv[1]);
	testHops(argv[2]);
	testStorage();
	testLink();
	testIsolationAndGates();
	testInitialKeyChange();
	testMigration();
	testUsb();
	std::printf("PASS: %u assertions; synthetic tests, not hardware interoperability\n", assertions);
}
