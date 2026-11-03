#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sc {

constexpr unsigned SLOT_COUNT = 4;
constexpr unsigned BOND_COUNT = 20;
constexpr unsigned MAX_PLAIN = 27;
constexpr uint64_t COUNTER_MASK = (UINT64_C(1) << 39) - 1;
extern const uint8_t DISCOVERY_ADDRESS[5];
extern const uint8_t BOOTSTRAP_KEY[16];

// Original D:1951 read queries complete SET only once a radio reply exists.
// AE/BA are later read-only string/chip queries and use the same USB gate.
constexpr bool featureQuery(uint8_t command)
{
	return command == 0x82 || command == 0x83 || command == 0x84 ||
	       (command >= 0x89 && command <= 0x8c) || command == 0xaa ||
	       command == 0xab || command == 0xae || command == 0xba;
}
// Receiver-local commands: unsupported legacy boot (90), PRNG entropy (96),
// and only the F0..FB XIAO extensions. A2..A6 still go to the peer.
constexpr bool localFeature(uint8_t command)
{
	return command == 0x90 || command == 0x96 || command == 0x9b ||
	       command == 0xa1 || command == 0xad || command == 0xaf ||
	       (command >= 0xb0 && command <= 0xb4) ||
	       (command >= 0xf0 && command <= 0xfb);
}
enum class QueryStatus : uint8_t { None, Pending, Ready, Offline };
enum class DisconnectReason : uint8_t {
	None = 0, Watchdog = 1, Host = 2, Replaced = 3, Crypto = 4, Erase = 5,
};

struct Packet {
	uint8_t size;
	uint8_t data[32];
};

struct JoinReplay {
	bool active;
	Packet request, acknowledgement;
};

struct Bond {
	bool used;
	uint8_t id[8];
	uint8_t key[16];
};

struct Database {
	uint8_t address[5];
	Bond bonds[BOND_COUNT];
};

using AesBlock = bool (*)(const uint8_t key[16], const uint8_t in[16],
			 uint8_t out[16]);

// Hardware ECB supplies AES; this portable layer implements the recovered CCM
// framing, including the one-byte AAD and direction in nonce byte four.
bool seal(AesBlock aes, const uint8_t key[16], const uint8_t iv[8],
	  uint64_t counter, bool downlink, uint8_t header,
	  const uint8_t *plain, unsigned size, Packet &wire);
bool open(AesBlock aes, const uint8_t key[16], const uint8_t iv[8],
	  uint64_t counter, bool downlink, const Packet &wire,
	  uint8_t plain[MAX_PLAIN], unsigned &size);
void nonce(uint8_t out[13], uint64_t counter, bool downlink,
	   const uint8_t iv[8]);
uint64_t uplinkCounter(uint64_t previous, uint8_t header);
uint8_t reverse8(uint8_t value);
uint32_t radioBase(const uint8_t address[5]);
void makeHops(const uint8_t address[5], uint8_t hops[7]);
uint32_t advanceHops(uint32_t now, uint32_t &deadline, uint8_t &index);
uint32_t crc32(const uint8_t *data, size_t size);

// Fixed-size, endian-independent two-page record. The final word is programmed
// last; an erased or torn record is never selected as a committed database.
constexpr unsigned STORE_SIZE = 528;
bool encodeDatabase(const Database &db, uint32_t generation,
		    uint8_t out[STORE_SIZE]);
bool decodeDatabase(const uint8_t in[STORE_SIZE], Database &db,
		    uint32_t &generation);
bool generationAfter(uint32_t a, uint32_t b);

struct Hooks {
	AesBlock aes;
	bool (*random)(uint8_t *data, size_t size);
	bool (*persist)(const Database &db);
	void (*input)(unsigned slot, const uint8_t report[64]);
	void (*desktop)(const uint8_t keyboard[8], const uint8_t mouse[4]);
};

struct Statistics {
	uint32_t rx;
	uint32_t authenticated;
	uint32_t mic_failures;
	uint32_t duplicates;
	uint32_t invalid;
	uint32_t queue_full;
	uint32_t storage_failures;
};

struct ReconnectStatistics {
	uint32_t lookup_hits, lookup_misses, restore_requests, restore_accepted;
	uint32_t restore_mic_failures, new_pairs, key_updates, commit_requests;
	uint32_t commit_deferred, commit_successes, commit_failures;
	uint32_t unsaved_disconnects, restore_fallbacks;
	uint8_t last_outcome;
};

class Receiver {
public:
	bool begin(const Database &db, const Hooks &hooks);
	void pairing(bool enable, bool auto_commit = false);
	bool pairing() const { return pairing_; }
	bool desktopEnabled() const { return desktop_enabled_; }
	// Clear input snapshots and publish neutral reports; preserve RF sessions.
	void resetInputs();
	bool connected(unsigned slot) const;
	bool pending(unsigned slot) const;
	// USB-task-safe epoch capture: one aligned volatile word read, no crypto.
	// Zero is invalid; begin() starts each slot at one, resetSlot() advances it.
	uint32_t sessionGeneration(unsigned slot) const;
	uint8_t activePipes() const;
	const Database &database() const { return db_; }
	const Statistics &statistics() const { return stats_; }
	const uint8_t *challenge() const { return challenge_; }
	void receive(unsigned pipe, const Packet &wire);
	void service();
	void hopTick();
	void advanceToHop(uint32_t absolute_hop);
	void resetHopClock() { processed_hops_ = 0; }
	// Deferred radio publication requires an authenticated phase witness.
	// false exposes the raw lane for synchronous/original-policy comparisons.
	Packet acknowledgement(unsigned pipe, unsigned peer_parity, bool require_phase = true) const;
	JoinReplay joinReplay(unsigned slot) const;
	bool feature(unsigned slot, const uint8_t *data, unsigned size);
	// Loop-task only, like feature(). A stale epoch returns false without side
	// effects; the single protocol owner keeps the check and dispatch together.
	bool featureForSession(unsigned slot, uint32_t generation,
			       const uint8_t *data, unsigned size);
	// Side-effect-free capacity probe, not command acceptance or query readiness.
	// Invalid frames return false; valid local/offline commands need no RF space.
	bool canQueueFeature(unsigned slot, const uint8_t *data, unsigned size) const;
	QueryStatus queryStatus(unsigned slot, const uint8_t *data, unsigned size) const;
	void peekFeature(unsigned slot, uint8_t reply[64], uint32_t &version) const;
	void consumeFeature(unsigned slot, uint32_t version);
	void disconnect(unsigned slot, DisconnectReason reason = DisconnectReason::Host);
	bool takeIdentityChange();
	bool takeBootloaderRequest();

private:
	struct Message {
		uint8_t type, size;
		uint8_t body[MAX_PLAIN];
	};
	struct Lane {
		bool valid;
		Packet wire;
	};
	struct Slot {
		bool connected, pending;
		bool awaiting_new_key;
		bool key_installed, key_ready, commit_requested;
		bool admission_confirmed, initial_control, usb_notified, count_dirty;
		Packet join_request, join_ack;
		Packet bootstrap_ack, key_update_request;
		uint8_t id[8], key[16], iv[8];
		uint64_t rx_counter, tx_counter;
		uint8_t phases, watchdog;
		Message queue[12];
		uint8_t head, count;
		Lane lane[2];
		uint8_t state[42], keyboard[8], mouse[4];
		uint8_t fragments[54], fragment_size, fragment_count;
		bool fragment_drop;
		uint8_t reply[64], last_command;
		uint8_t wireless_reply[64];
		bool reply_ready, wireless_reply_ready, local_feature, feature_waiting;
		uint8_t request[64], request_size;
		uint32_t reply_version;
	};
	struct DisconnectDiagnostic {
		DisconnectReason reason;
		// Final pre-reset watchdog, queue count, lane/phase bits, RX/TX low8.
		uint8_t state[5];
	};
	Database db_{};
	Hooks hooks_{};
	Slot slots_[SLOT_COUNT]{};
	// Separate from Slot: neither its memset nor USB publication resets these.
	alignas(4) volatile uint32_t session_generations_[SLOT_COUNT]{};
	DisconnectDiagnostic disconnect_diagnostics_[SLOT_COUNT]{};
	uint32_t disconnect_counts_[5]{};
	Statistics stats_{};
	ReconnectStatistics reconnect_{};
	uint8_t challenge_[8]{};
	uint8_t lookup_key_[16]{};
	uint8_t lookup_id_[8]{};
	bool lookup_valid_ = false;
	bool challenge_ready_ = false;
	bool pairing_ = false;
	bool auto_commit_ = false;
	bool desktop_enabled_ = true;
	uint8_t watchdog_phase_ = 0;
	uint32_t usb_sequence_ = 0;
	uint32_t processed_hops_ = 0;
	enum class Transfer : uint8_t { None, Export, Import };
	Transfer transfer_ = Transfer::None;
	Database transfer_db_{};
	uint32_t transfer_token_ = 0, committed_token_ = 0;
	uint32_t transfer_received_ = 0, transfer_age_ = 0;
	uint8_t transfer_expected_ = 0;
	bool transfer_pairing_ = false, transfer_auto_commit_ = false;
	bool identity_changed_ = false;
	bool bootloader_ready_ = false;

	unsigned freeSlot() const;
	unsigned connectedCount() const;
	bool refreshChallenge();
	void join(const Packet &wire);
	bool enqueue(unsigned slot, uint8_t type, const uint8_t *body,
		     unsigned size);
	void prepare(unsigned slot);
	void ackPhase(Slot &slot, uint8_t header);
	void resetTransport(Slot &slot);
	void resetSlot(unsigned slot);
	static void transportSnapshot(const Slot &slot, uint8_t out[5]);
	bool control(unsigned slot, const uint8_t *body, unsigned size, const Packet &wire);
	void initialControls(unsigned slot);
	void countsChanged();
	void report(unsigned slot, const uint8_t *body, unsigned size);
	void fragment(unsigned slot, unsigned type, const uint8_t *body,
		      unsigned size);
	void stateEvent(unsigned slot, uint8_t state);
	void neutralInput(unsigned slot);
	void desktopReport();
	bool storeBond(const uint8_t id[8], const uint8_t key[16]);
	bool commitBond(unsigned slot);
	int findBond(const uint8_t id[8]) const;
	void transferFeature(unsigned slot, const uint8_t *data, unsigned size);
	void transferReply(unsigned slot, uint8_t command, uint8_t status,
			   unsigned payload_size = 1);
	void finishTransfer(bool restore_pairing);
};

} // namespace sc
