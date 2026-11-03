#include "protocol.h"
#include "radio_policy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#endif

namespace {

unsigned assertions;
#define RADIO_CHECK(value) do { ++assertions; if (!(value)) { \
	std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #value); \
	std::exit(1); } } while (0)

// Use the same host AES dependencies as protocol_test.cpp, without depending
// on that translation unit's private hooks. CCM and transport are production.
bool aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
#ifdef _WIN32
	BCRYPT_ALG_HANDLE provider = nullptr;
	BCRYPT_KEY_HANDLE handle = nullptr;
	RADIO_CHECK(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0);
	RADIO_CHECK(BCryptSetProperty(provider, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
		ULONG(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0) == 0);
	RADIO_CHECK(BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0,
		const_cast<PUCHAR>(key), 16, 0) == 0);
	ULONG count = 0;
	bool result = BCryptEncrypt(handle, const_cast<PUCHAR>(in), 16, nullptr,
		nullptr, 0, out, 16, &count, 0) == 0 && count == 16;
	BCryptDestroyKey(handle);
	BCryptCloseAlgorithmProvider(provider, 0);
	return result;
#else
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	RADIO_CHECK(ctx);
	int count = 0;
	bool result = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
		EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
		EVP_EncryptUpdate(ctx, out, &count, in, 16) == 1 && count == 16;
	EVP_CIPHER_CTX_free(ctx);
	return result;
#endif
}

bool randomBytes(uint8_t *data, size_t size)
{
	static uint8_t next;
	for (size_t i = 0; i < size; ++i)
		data[i] = ++next;
	return true;
}

bool persist(const sc::Database &)
{
	RADIO_CHECK(false); // Restored sessions must not write a new bond here.
	return false;
}

void input(unsigned, const uint8_t[64]) {}

sc::Database database()
{
	sc::Database db{};
	const uint8_t address[5] = {0x28, 0x36, 0x57, 0x62, 0x94};
	std::memcpy(db.address, address, sizeof(address));
	for (unsigned slot = 0; slot < 2; ++slot) {
		sc::Bond &bond = db.bonds[slot];
		bond.used = true;
		for (unsigned i = 0; i < 8; ++i)
			bond.id[i] = uint8_t(0x30 + slot * 16 + i);
		for (unsigned i = 0; i < 16; ++i)
			bond.key[i] = uint8_t(0x90 + slot * 16 + i);
	}
	return db;
}

struct Peer {
	uint8_t key[16]{}, iv[8]{};
	unsigned pipe = 2;
	uint64_t up = 1;
	uint64_t down = 0;
	std::vector<sc::Packet> messages;
	std::vector<uint8_t> types;

	sc::Packet uplink() const
	{
		// Retry one authenticated uplink at counter1. P:15dc can change its
		// bit2 as downlinks arrive, without changing the ciphertext/MIC.
		const uint8_t info[8] = {4};
		sc::Packet wire{};
		uint8_t header = uint8_t(1 | ((up & 7) << 4) | ((down & 1) << 2));
		RADIO_CHECK(sc::seal(aes, key, iv, up, false, header, info, sizeof(info), wire));
		return wire;
	}

	bool accept(const sc::Packet &wire)
	{
		// P:0ec4 accepts only the next downlink counter, never a +3 search.
		if (wire.size <= 1 || ((wire.data[0] >> 2) & 1) == (down & 1))
			return false;
		uint8_t plain[sc::MAX_PLAIN];
		unsigned size;
		if (!sc::open(aes, key, iv, down + 1, true, wire, plain, size))
			return false;
		RADIO_CHECK((wire.data[0] & 3) != 0);
		sc::Packet message{};
		message.size = uint8_t(size);
		std::memcpy(message.data, plain, size);
		messages.push_back(message);
		types.push_back(wire.data[0] & 3);
		++down;
		return true;
	}

	void feedback(const sc::Packet &wire)
	{
		// P:2c7a..2cb6 calls 1194 for a one-byte feedback ACK, or ONLY
		// after 0ec4 authenticates a new downlink. Rejected ciphertext does
		// not acknowledge the uplink, even if header bits6:4 look correct.
		if (!wire.size || (wire.size > 1 && !accept(wire)))
			return;
		if (((wire.data[0] >> 4) & 7) == (up & 7))
			++up;
	}
};

Peer restore(sc::Receiver &receiver, unsigned bond_index)
{
	const sc::Bond &bond = receiver.database().bonds[bond_index];
	sc::Packet lookup{};
	lookup.size = 9;
	std::memcpy(lookup.data + 1, bond.id, 8);
	receiver.receive(1, lookup);
	sc::Packet ack = receiver.acknowledgement(1, 0);
	RADIO_CHECK(ack.size == 22);
	Peer peer;
	peer.pipe = ack.data[9] & 7;
	RADIO_CHECK(peer.pipe == bond_index + 2);
	std::memcpy(peer.key, bond.key, 16);
	std::memcpy(peer.iv, ack.data + 1, 4);
	for (unsigned i = 0; i < 4; ++i)
		peer.iv[4 + i] = uint8_t(0xa0 + bond_index * 4 + i);
	uint8_t plain[17];
	std::memcpy(plain, receiver.database().address, 5);
	std::memcpy(plain + 5, bond.id, 8);
	std::memcpy(plain + 13, peer.iv + 4, 4);
	sc::Packet join{};
	RADIO_CHECK(sc::seal(aes, peer.key, ack.data + 1, 0, false, 2, plain, sizeof(plain), join));
	receiver.receive(1, join);
	RADIO_CHECK(receiver.connected(peer.pipe - 2));
	RADIO_CHECK(!receiver.pending(peer.pipe - 2));
	return peer;
}

struct Fixture {
	sc::Receiver receiver;
	Peer peer;

	Fixture()
	{
		const sc::Hooks hooks = {aes, randomBytes, persist, input, nullptr};
		RADIO_CHECK(receiver.begin(database(), hooks));
		peer = restore(receiver, 0);
		receiver.service(); // counter1=04, counter2=03,count1 occupy both lanes.
		restore(receiver, 1);
		receiver.service(); // Another join queues counter3=03,count2 behind them.
	}
};

// Only the ISR/deferred-work boundary is modeled. Lane preparation, phase
// release, counters, MICs and ACK selection use the actual production code.
// No receive() or service() runs inside arrive(): the ACK uses the preceding
// published epoch, just as radio.cpp does before main drains its RX ring.
class DeferredRadio {
	struct Rx { unsigned pipe; sc::Packet wire; } queue[32]{};
	unsigned head = 0, tail = 0;
	sc::Receiver &receiver;
	radio::ResponseSnapshot published{};
	bool legacy_ack;
	bool legacy_phase;

public:
	struct Result {
		bool retained = false, acknowledged = false;
		sc::Packet ack{};
	};

	explicit DeferredRadio(sc::Receiver &target, bool legacy = false, bool old_phase = true)
		: receiver(target), legacy_ack(legacy), legacy_phase(old_phase)
	{
		publish();
	}

	void publish()
	{
		for (unsigned pipe = 0; pipe < 6; ++pipe)
			for (unsigned parity = 0; parity < 2; ++parity)
				published.acknowledgements[pipe][parity] = receiver.acknowledgement(pipe, parity, !legacy_phase);
		for (unsigned i = 0; i < sc::SLOT_COUNT; ++i)
			published.joins[i] = receiver.joinReplay(i);
		published.mask = receiver.activePipes();
	}

	Result arrive(unsigned pipe, const sc::Packet &wire, uint8_t s1 = 1, bool crc_valid = true)
	{
		Result result;
		bool valid = crc_valid && wire.size && wire.size <= 32 && pipe < 6;
		unsigned next = (head + 1) % 32;
		if (valid && next != tail) {
			queue[head] = {pipe, wire};
			head = next;
			result.retained = true;
		}
		// Legacy mode deliberately reproduces revision7's missing retention
		// gate; the fixed scenario uses the same helper as the production ISR.
		result.acknowledged = legacy_ack ? radio::wantsAck(valid, s1) :
			radio::shouldAck(valid, result.retained, s1);
		if (result.acknowledged)
			result.ack = published.select(pipe, (wire.data[0] >> 2) & 1, wire.data, wire.size);
		return result;
	}

	void fill()
	{
		RADIO_CHECK(head == tail);
		RADIO_CHECK(published.mask & 2);
		sc::Packet lookup{};
		lookup.size = 9;
		std::memcpy(lookup.data + 1, receiver.database().bonds[1].id, 8);
		// Admission polls occupy all 31 usable entries without changing the
		// business pipe's ACK-phase history when they are eventually drained.
		for (unsigned i = 0; i < 31; ++i)
			RADIO_CHECK(arrive(1, lookup).retained);
	}

	void drain()
	{
		while (tail != head) {
			const Rx &rx = queue[tail];
			receiver.receive(rx.pipe, rx.wire);
			tail = (tail + 1) % 32;
		}
	}

	void service()
	{
		drain();
		receiver.service();
		publish();
	}
};

void checkInitialMessages(const Peer &peer)
{
	RADIO_CHECK(peer.messages.size() >= 2);
	RADIO_CHECK(peer.types[0] == 1 && peer.types[1] == 1);
	RADIO_CHECK(peer.messages[0].size == 8 && peer.messages[0].data[0] == 4);
	RADIO_CHECK(peer.messages[1].size == 2 && peer.messages[1].data[0] == 3);
	RADIO_CHECK(peer.messages[1].data[1] == 1);
}

void testLegacyOverflowDeadlock()
{
	Fixture fixture;
	DeferredRadio driver(fixture.receiver, true);
	Peer &peer = fixture.peer;
	driver.fill();
	auto dropped = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(!dropped.retained && dropped.acknowledged);
	RADIO_CHECK(peer.accept(dropped.ack) && peer.down == 1);
	driver.service();
	auto even = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(even.retained && even.acknowledged);
	RADIO_CHECK(peer.accept(even.ack) && peer.down == 2);
	auto stale = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(stale.retained && stale.acknowledged);
	RADIO_CHECK(!peer.accept(stale.ack));
	driver.service();
	checkInitialMessages(peer);
	for (unsigned i = 0; i < 8; ++i) {
		auto retry = driver.arrive(peer.pipe, peer.uplink());
		RADIO_CHECK(retry.retained && retry.acknowledged);
		RADIO_CHECK(retry.ack.size > 1 && !peer.accept(retry.ack));
		uint8_t plain[sc::MAX_PLAIN];
		unsigned size;
		RADIO_CHECK(sc::open(aes, peer.key, peer.iv, 1, true, retry.ack, plain, size));
		driver.service(); // counter1 stays in lane1; queued counter3 cannot advance.
	}
	RADIO_CHECK(peer.down == 2 && peer.messages.size() == 2);
	RADIO_CHECK(fixture.receiver.statistics().mic_failures == 0);
	for (unsigned i = 0; i < 41 * 8; ++i)
		fixture.receiver.hopTick();
	RADIO_CHECK(!fixture.receiver.connected(peer.pipe - 2));
}

void testRetainedDeferredProgress()
{
	Fixture fixture;
	DeferredRadio driver(fixture.receiver);
	Peer &peer = fixture.peer;
	driver.fill();
	auto dropped = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(!dropped.retained && !dropped.acknowledged);
	RADIO_CHECK(dropped.ack.size == 0 && peer.down == 0);
	driver.service();
	uint32_t authenticated = fixture.receiver.statistics().authenticated;
	auto odd = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(odd.retained && odd.acknowledged && peer.accept(odd.ack));
	auto even = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(even.retained && even.acknowledged && peer.accept(even.ack));
	RADIO_CHECK(peer.down == 2);
	// Main has not seen either phase yet. The old epoch still offers counter1;
	// the peer rejects it, and the retained RX must still reach ackPhase().
	auto stale = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(stale.retained && stale.acknowledged && !peer.accept(stale.ack));
	RADIO_CHECK(fixture.receiver.statistics().authenticated == authenticated);
	driver.service(); // Drain crosses ring index31->0, then prepare/publish counter3.
	RADIO_CHECK(fixture.receiver.statistics().authenticated == authenticated + 3);
	RADIO_CHECK(fixture.receiver.statistics().duplicates == 2);
	auto third = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(third.retained && third.acknowledged && peer.accept(third.ack));
	RADIO_CHECK(peer.down == 3 && peer.messages.size() == 3);
	checkInitialMessages(peer);
	RADIO_CHECK(peer.messages[2].size == 2 && peer.messages[2].data[0] == 3);
	RADIO_CHECK(peer.messages[2].data[1] == 2);
	auto confirmation = driver.arrive(peer.pipe, peer.uplink());
	RADIO_CHECK(confirmation.retained && confirmation.acknowledged);
	RADIO_CHECK(confirmation.ack.size == 1);
	driver.service();
	RADIO_CHECK(fixture.receiver.acknowledgement(peer.pipe, 0).size == 1);
	RADIO_CHECK(fixture.receiver.acknowledgement(peer.pipe, 1).size == 1);
	RADIO_CHECK(fixture.receiver.connected(peer.pipe - 2));
	RADIO_CHECK(fixture.receiver.statistics().duplicates == 4);
	RADIO_CHECK(fixture.receiver.statistics().mic_failures == 0);
}

void testAdmissionGates()
{
	Fixture fixture;
	DeferredRadio driver(fixture.receiver);
	sc::Packet wire = fixture.peer.uplink();
	auto invalid = driver.arrive(fixture.peer.pipe, wire, 1, false);
	RADIO_CHECK(!invalid.retained && !invalid.acknowledged);
	invalid = driver.arrive(6, wire);
	RADIO_CHECK(!invalid.retained && !invalid.acknowledged);
	sc::Packet bad_width = wire;
	bad_width.size = 33;
	invalid = driver.arrive(fixture.peer.pipe, bad_width);
	RADIO_CHECK(!invalid.retained && !invalid.acknowledged);
	bad_width.size = 0;
	invalid = driver.arrive(fixture.peer.pipe, bad_width);
	RADIO_CHECK(!invalid.retained && !invalid.acknowledged);
	uint32_t authenticated = fixture.receiver.statistics().authenticated;
	auto noack = driver.arrive(fixture.peer.pipe, wire, 0);
	RADIO_CHECK(noack.retained && !noack.acknowledged);
	driver.service();
	RADIO_CHECK(fixture.receiver.statistics().authenticated == authenticated + 1);
	RADIO_CHECK(fixture.peer.down == 0); // No-ACK is still delivered to transport.
	for (uint8_t pid = 0; pid < 4; ++pid) {
		uint8_t requested = uint8_t((pid << 1) | 1);
		RADIO_CHECK(radio::shouldAck(true, true, requested));
		RADIO_CHECK(!radio::shouldAck(true, false, requested));
		RADIO_CHECK(!radio::shouldAck(false, true, requested));
		RADIO_CHECK(!radio::shouldAck(false, false, requested));
		RADIO_CHECK(!radio::shouldAck(true, true, uint8_t(pid << 1)));
	}
}

void testRetainedButUnauthenticatedPhase(bool legacy)
{
	sc::Receiver receiver;
	const sc::Hooks hooks = {aes, randomBytes, persist, input, nullptr};
	RADIO_CHECK(receiver.begin(database(), hooks));
	Peer peer = restore(receiver, 0);
	sc::Packet stale = peer.uplink(); // Previously emitted uplink counter1.
	peer.up = 2;
	receiver.receive(peer.pipe, peer.uplink());
	receiver.service(); // New lanes clear phase witnesses after the prior RX.
	restore(receiver, 1);
	receiver.service(); // Third message queued for this peer.
	DeferredRadio driver(receiver, false, legacy);
	// RX is retained and has a valid MIC for old counter1, but cannot be
	// authenticated under receiver counter2's forward window. The old ISR
	// nevertheless lets the peer receive downlink1 from this observation.
	auto first = driver.arrive(peer.pipe, stale);
	RADIO_CHECK(first.retained && first.acknowledged);
	peer.feedback(first.ack);
	driver.service();
	RADIO_CHECK(receiver.statistics().mic_failures == 1);
	unsigned max_steps = legacy ? 400 : 3000;
	for (unsigned step = 0; step < max_steps && receiver.connected(0); ++step) {
		auto result = driver.arrive(peer.pipe, peer.uplink());
		RADIO_CHECK(result.retained && result.acknowledged);
		peer.feedback(result.ack);
		driver.service();
		receiver.hopTick();
	}
	if (legacy) {
		RADIO_CHECK(!receiver.connected(0));
		RADIO_CHECK(peer.down == 2 && peer.messages.size() == 2);
		RADIO_CHECK(receiver.statistics().duplicates > 300);
	} else {
		RADIO_CHECK(receiver.connected(0));
		// One old feedback ACK plus the updated publication can take two polls.
		RADIO_CHECK(peer.down >= 3 && peer.up > 1000);
		checkInitialMessages(peer);
		RADIO_CHECK(peer.messages[2].data[0] == 3 && peer.messages[2].data[1] == 2);
		RADIO_CHECK(receiver.statistics().queue_full == 0);
	}
}

void testDuplexLoadWithLoss()
{
	sc::Receiver receiver;
	const sc::Hooks hooks = {aes, randomBytes, persist, input, nullptr};
	RADIO_CHECK(receiver.begin(database(), hooks));
	Peer peer = restore(receiver, 0);
	receiver.service();
	DeferredRadio driver(receiver, false, false);
	sc::Packet previous{};
	std::vector<std::vector<uint8_t>> expected, delivered;
	for (unsigned step = 0; step < 4000; ++step) {
		if (step < 3500 && step % 50 == 0) {
			uint8_t pulse[] = {0x8f, 7, uint8_t((step / 50) & 1),
				uint8_t(step), uint8_t(step >> 8), 0x90, 1, 2, 0};
			RADIO_CHECK(receiver.feature(0, pulse, sizeof(pulse)));
			expected.emplace_back(pulse, pulse + sizeof(pulse));
		}
		sc::Packet current = peer.uplink();
		bool stale = step % 31 == 0 && previous.size;
		auto result = driver.arrive(peer.pipe, stale ? previous : current);
		RADIO_CHECK(result.retained && result.acknowledged);
		if (step % 17) peer.feedback(result.ack); // Lost ACK, not lost retained RX.
		previous = current;
		if (step % 3 == 2) driver.service(); // Multiple replies per published epoch.
		receiver.hopTick();
		RADIO_CHECK(receiver.connected(0));
	}
	driver.service();
	for (size_t i = 0; i < peer.messages.size(); ++i)
		if (peer.types[i] == 3)
			delivered.emplace_back(peer.messages[i].data, peer.messages[i].data + peer.messages[i].size);
	RADIO_CHECK(delivered == expected && delivered.size() == 70);
	RADIO_CHECK(peer.up > 500 && receiver.statistics().duplicates > 0);
	RADIO_CHECK(receiver.statistics().queue_full == 0);
}

} // namespace

void testRadioRetention()
{
	assertions = 0;
	testLegacyOverflowDeadlock();
	testRetainedDeferredProgress();
	testAdmissionGates();
	testRetainedButUnauthenticatedPhase(true);
	testRetainedButUnauthenticatedPhase(false);
	testDuplexLoadWithLoss();
	std::printf("PASS: %u radio-retention assertions; legacy overflow deadlock, retained deferred ACK progress and admission gates (software only)\n",
		assertions);
}
