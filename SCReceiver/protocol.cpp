#include "protocol.h"
#include <string.h>

namespace sc {

static const uint8_t BUILD_TAG[4] = {0x55, 0xf7, 0x16, 0x0e};

static void put32(uint8_t *out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out[i] = uint8_t(value >> (8 * i));
}

static uint32_t get32(const uint8_t *in)
{
	return uint32_t(in[0]) | (uint32_t(in[1]) << 8) |
	       (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}

bool Receiver::begin(const Database &db, const Hooks &hooks)
{
	if (!hooks.aes || !hooks.random || !hooks.persist || !hooks.input ||
	    (db.address[0] & 7))
		return false;
	db_ = db;
	hooks_ = hooks;
	memset(slots_, 0, sizeof(slots_));
	memset(&stats_, 0, sizeof(stats_));
	pairing_ = false;
	desktop_enabled_ = true;
	watchdog_phase_ = 0;
	usb_sequence_ = 0;
	transfer_ = Transfer::None;
	transfer_token_ = committed_token_ = 0;
	transfer_received_ = transfer_age_ = 0;
	identity_changed_ = false;
	memset(&transfer_db_, 0, sizeof(transfer_db_));
	return refreshChallenge();
}

bool Receiver::refreshChallenge()
{
	uint8_t fresh[8];
	challenge_ready_ = hooks_.random(fresh, sizeof(fresh));
	lookup_valid_ = false;
	memset(lookup_key_, 0, sizeof(lookup_key_));
	if (challenge_ready_)
		memcpy(challenge_, fresh, sizeof(fresh));
	return challenge_ready_;
}

unsigned Receiver::freeSlot() const
{
	for (unsigned i = 0; i < SLOT_COUNT; ++i)
		if (!slots_[i].connected)
			return i;
	return SLOT_COUNT;
}

unsigned Receiver::connectedCount() const
{
	unsigned result = 0;
	for (unsigned i = 0; i < SLOT_COUNT; ++i)
		result += slots_[i].connected ? 1 : 0;
	return result;
}

bool Receiver::connected(unsigned slot) const
{
	return slot < SLOT_COUNT && slots_[slot].connected;
}

bool Receiver::pending(unsigned slot) const
{
	return connected(slot) && slots_[slot].pending;
}

void Receiver::pairing(bool enable)
{
	if (transfer_ == Transfer::Import)
		return;
	pairing_ = enable && challenge_ready_ && freeSlot() < SLOT_COUNT;
}

uint8_t Receiver::activePipes() const
{
	if (transfer_ == Transfer::Import)
		return 0;
	uint8_t mask = 0;
	if (challenge_ready_ && freeSlot() < SLOT_COUNT)
		mask = pairing_ ? 3 : 2;
	for (unsigned slot = 0; slot < SLOT_COUNT; ++slot)
		if (slots_[slot].connected)
			mask |= 1u << (slot + 2);
	return mask;
}

Packet Receiver::acknowledgement(unsigned pipe, unsigned peer_parity) const
{
	Packet packet{};
	if (pipe < 2) {
		unsigned slot = freeSlot();
		if (!challenge_ready_ || slot == SLOT_COUNT)
			return packet;
		packet.size = 22;
		packet.data[0] = 4;
		memcpy(packet.data + 1, challenge_, 8);
		memcpy(packet.data + 9, db_.address, 5);
		if (pipe == 1)
			packet.data[9] |= uint8_t(slot + 2);
		return packet;
	}
	if (pipe >= SLOT_COUNT + 2 || !slots_[pipe - 2].connected)
		return packet;
	const Slot &slot = slots_[pipe - 2];
	// State6 switches keys only after an authenticated bootstrap downlink.
	// Keep it available even when deferred RX work batches join and rekey.
	if (slot.awaiting_new_key && !(peer_parity & 1))
		return slot.bootstrap_ack;
	unsigned lane = 1 - (peer_parity & 1);
	if (slot.lane[lane].valid)
		return slot.lane[lane].wire;
	packet.size = 1;
	packet.data[0] = uint8_t((slot.rx_counter & 7) << 4);
	return packet;
}

int Receiver::findBond(const uint8_t id[8]) const
{
	for (unsigned i = 0; i < BOND_COUNT; ++i)
		if (db_.bonds[i].used && !memcmp(id, db_.bonds[i].id, 8))
			return int(i);
	return -1;
}

bool Receiver::storeBond(const uint8_t id[8], const uint8_t key[16])
{
	int index = findBond(id);
	if (index < 0)
		for (unsigned i = 0; i < BOND_COUNT; ++i)
			if (!db_.bonds[i].used) {
				index = int(i);
				break;
			}
	if (index < 0) {
		++stats_.storage_failures;
		return false;
	}
	Database replacement = db_;
	Bond &bond = replacement.bonds[index];
	bond.used = true;
	memcpy(bond.id, id, 8);
	memcpy(bond.key, key, 16);
	if (!hooks_.persist(replacement)) {
		++stats_.storage_failures;
		return false;
	}
	db_ = replacement;
	committed_token_ = 0;
	return true;
}

void Receiver::join(const Packet &wire)
{
	if (transfer_ == Transfer::Import || !challenge_ready_ || freeSlot() == SLOT_COUNT)
		return;
	uint8_t header = wire.data[0];
	if (header == 0 && wire.size >= 9) {
		int bond = findBond(wire.data + 1);
		lookup_valid_ = bond >= 0;
		if (lookup_valid_)
			memcpy(lookup_key_, db_.bonds[bond].key, 16);
		return;
	}
	if ((header != 1 || !pairing_) && (header != 2 || !lookup_valid_))
		return;
	uint8_t plain[MAX_PLAIN];
	unsigned size;
	const uint8_t *key = header == 1 ? BOOTSTRAP_KEY : lookup_key_;
	if (!open(hooks_.aes, key, challenge_, 0, false, wire, plain, size)) {
		++stats_.mic_failures;
		return;
	}
	if (size < 17 || memcmp(plain, db_.address, 5)) {
		++stats_.invalid;
		refreshChallenge();
		return;
	}
	// A repeated connection replaces its live slot only after authentication.
	// The advertised pipe must remain free until this join is accepted.
	unsigned index = freeSlot();
	uint8_t accepted_key[16], accepted_iv[8];
	memcpy(accepted_key, key, 16);
	memcpy(accepted_iv, challenge_, 4);
	memcpy(accepted_iv + 4, plain + 13, 4);
	for (unsigned i = 0; i < SLOT_COUNT; ++i)
		if (slots_[i].connected && !memcmp(slots_[i].id, plain + 5, 8))
			disconnect(i);
	Slot &slot = slots_[index];
	uint32_t version = slot.reply_version + 1;
	memset(&slot, 0, sizeof(slot));
	slot.reply_version = version;
	slot.connected = true;
	slot.pending = header == 1;
	slot.watchdog = 41;
	memcpy(slot.id, plain + 5, 8);
	memcpy(slot.key, accepted_key, 16);
	memcpy(slot.iv, accepted_iv, 8);
	pairing_ = false;
	refreshChallenge();
	initialControls(index);
	stateEvent(index, slot.pending ? 3 : 2);
	++stats_.authenticated;
}

void Receiver::resetTransport(Slot &slot)
{
	slot.rx_counter = slot.tx_counter = 0;
	slot.phases = slot.head = slot.count = 0;
	slot.lane[0].valid = slot.lane[1].valid = false;
	slot.fragment_count = slot.fragment_size = 0;
	slot.fragment_drop = false;
	slot.feature_waiting = false;
}

bool Receiver::enqueue(unsigned index, uint8_t type, const uint8_t *body,
		       unsigned size)
{
	if (index >= SLOT_COUNT || !size || size > MAX_PLAIN)
		return false;
	Slot &slot = slots_[index];
	if (slot.count == 12) {
		++stats_.queue_full;
		return false;
	}
	Message &message = slot.queue[(slot.head + slot.count) % 12];
	message.type = type & 3;
	message.size = uint8_t(size);
	memcpy(message.body, body, size);
	++slot.count;
	return true;
}

void Receiver::initialControls(unsigned index)
{
	uint8_t info[8] = {4};
	memcpy(info + 4, BUILD_TAG, 4);
	enqueue(index, 1, info, sizeof(info));
	uint8_t count[2] = {3, uint8_t(connectedCount())};
	enqueue(index, 1, count, sizeof(count));
}

void Receiver::prepare(unsigned index)
{
	Slot &slot = slots_[index];
	if (!slot.connected || !slot.count)
		return;
	unsigned lane = unsigned((slot.tx_counter + 1) & 1);
	if (slot.lane[lane].valid)
		return;
	Message &message = slot.queue[slot.head];
	uint64_t counter = (slot.tx_counter + 1) & COUNTER_MASK;
	uint8_t header = message.type | uint8_t((slot.rx_counter & 7) << 4) |
			 uint8_t(lane << 2);
	Packet wire;
	if (!seal(hooks_.aes, slot.key, slot.iv, counter, true, header,
		  message.body, message.size, wire))
		return;
	slot.tx_counter = counter;
	slot.lane[lane].wire = wire;
	slot.lane[lane].valid = true;
	slot.phases &= uint8_t(~(4u << lane));
	slot.head = (slot.head + 1) % 12;
	--slot.count;
}

void Receiver::service()
{
	for (unsigned i = 0; i < SLOT_COUNT; ++i) {
		prepare(i);
		prepare(i);
	}
}

void Receiver::ackPhase(Slot &slot, uint8_t header)
{
	if (header & 4) {
		slot.phases |= 4;
		if (slot.phases & 8) {
			slot.lane[1].valid = false;
			slot.phases &= uint8_t(~8);
		}
	} else {
		slot.phases |= 8;
		if (slot.phases & 4) {
			slot.lane[0].valid = false;
			slot.phases &= uint8_t(~4);
		}
	}
}

void Receiver::receive(unsigned pipe, const Packet &wire)
{
	++stats_.rx;
	if (pipe >= 6 || !wire.size || wire.size > 32) {
		++stats_.invalid;
		return;
	}
	if (pipe == 0)
		return;
	if (pipe == 1) {
		join(wire);
		return;
	}
	Slot &slot = slots_[pipe - 2];
	if (!slot.connected)
		return;
	uint8_t header = wire.data[0];
	uint64_t candidate = uplinkCounter(slot.rx_counter, header);
	uint8_t plain[MAX_PLAIN];
	unsigned size;
	if (!open(hooks_.aes, slot.key, slot.iv, candidate, false,
		  wire, plain, size)) {
		if (slot.awaiting_new_key && wire.size == slot.key_update_request.size &&
		    !memcmp(wire.data, slot.key_update_request.data, wire.size)) {
			// This exact ciphertext/MIC was already authenticated before the
			// key switch. Do not reinstall the key or allocate a new counter.
			++stats_.authenticated;
			++stats_.duplicates;
			return;
		}
		++stats_.mic_failures;
		return;
	}
	++stats_.authenticated;
	slot.awaiting_new_key = false;
	if (candidate == slot.rx_counter) {
		++stats_.duplicates;
		ackPhase(slot, header);
		return;
	}
	unsigned index = pipe - 2;
	switch (header & 3) {
	case 0:
		report(index, plain, size);
		break;
	case 1:
		control(index, plain, size, wire);
		break;
	case 2:
	case 3:
		fragment(index, header & 3, plain, size);
		break;
	}
	ackPhase(slot, header);
	// This commit deliberately follows key-update's reset, as in D:22b2.
	slot.rx_counter = candidate;
	for (unsigned lane = 0; lane < 2; ++lane)
		if (slot.lane[lane].valid)
			slot.lane[lane].wire.data[0] =
				(slot.lane[lane].wire.data[0] & 0x8f) |
				uint8_t((candidate & 7) << 4);
	slot.watchdog = 41;
}

void Receiver::control(unsigned index, const uint8_t *body, unsigned size, const Packet &wire)
{
	if (size == 17 && body[0] == 1) {
		Slot &slot = slots_[index];
		bool bootstrap = slot.pending && slot.rx_counter == 0 &&
			!memcmp(slot.key, BOOTSTRAP_KEY, 16);
		Packet old_ack{};
		if (bootstrap) {
			uint8_t info[8] = {4};
			memcpy(info + 4, BUILD_TAG, 4);
			if (!seal(hooks_.aes, slot.key, slot.iv, 1, true, 5,
				  info, sizeof(info), old_ack))
				return;
		}
		memcpy(slot.key, body + 1, 16);
		resetTransport(slot);
		if (bootstrap) {
			slot.bootstrap_ack = old_ack;
			slot.key_update_request = wire;
			slot.awaiting_new_key = true;
		}
		initialControls(index);
	}
}

void Receiver::stateEvent(unsigned index, uint8_t state)
{
	uint8_t report[64] = {1, 0, 3, 1, state};
	hooks_.input(index, report);
}

void Receiver::disconnect(unsigned index)
{
	if (index >= SLOT_COUNT || !slots_[index].connected)
		return;
	uint32_t version = slots_[index].reply_version + 1;
	memset(&slots_[index], 0, sizeof(Slot));
	slots_[index].reply_version = version;
	stateEvent(index, 1);
	uint8_t neutral[64] = {1, 0, 1, 60};
	put32(neutral + 4, ++usb_sequence_);
	hooks_.input(index, neutral);
	desktopReport();
	refreshChallenge();
}

void Receiver::hopTick()
{
	// A stalled browser cannot leave the radio muted or hold stale keys in
	// the staging buffer indefinitely. 20000 nominal hops are 30 seconds.
	if (transfer_ != Transfer::None && ++transfer_age_ >= 20000)
		finishTransfer(true);
	if (watchdog_phase_ >= 2 && watchdog_phase_ < 6) {
		Slot &slot = slots_[watchdog_phase_ - 2];
		if (slot.connected && slot.watchdog && !--slot.watchdog)
			disconnect(watchdog_phase_ - 2);
	}
	watchdog_phase_ = (watchdog_phase_ + 1) & 7;
}

void Receiver::report(unsigned index, const uint8_t *body, unsigned size)
{
	if (size < 2) {
		++stats_.invalid;
		return;
	}
	uint8_t output[64] = {1, 0};
	if (body[0] == 0 && body[1] == 0x1f) {
		memcpy(output + 2, body + 2, size - 2);
		hooks_.input(index, output);
		return;
	}
	unsigned a = body[0] & 31, k = body[0] >> 5, m = body[1] >> 5;
	if (a > 16 || k > 7 || m > 4 || 2 + a + k + m > size) {
		++stats_.invalid;
		return;
	}
	static const uint8_t state_order[16] = {
		0, 1, 2, 12, 13, 14, 15, 8, 9, 10, 11, 3, 4, 5, 6, 7,
	};
	static const uint8_t keyboard_order[8] = {2, 0, 3, 4, 5, 6, 7, 1};
	static const uint8_t mouse_order[4] = {1, 2, 0, 3};
	Slot &slot = slots_[index];
	memset(slot.state, 0, 16);
	memset(slot.keyboard, 0, 8);
	memset(slot.mouse, 0, 4);
	unsigned offset = 2;
	for (unsigned i = 0; i < a; ++i)
		slot.state[state_order[i]] = body[offset++];
	for (unsigned i = 0; i < k; ++i)
		slot.keyboard[keyboard_order[i]] = body[offset++];
	for (unsigned i = 0; i < m; ++i)
		slot.mouse[mouse_order[i]] = body[offset++];
	if (offset < size) {
		uint8_t patch = body[offset++];
		if (patch & 0x20)
			memset(slot.state + 16, 0, 26);
		unsigned start = 16 + (patch & 31);
		if (start + size - offset <= 42)
			memcpy(slot.state + start, body + offset, size - offset);
	}
	output[2] = 1;
	output[3] = 60;
	put32(output + 4, ++usb_sequence_);
	memcpy(output + 8, slot.state, 42);
	hooks_.input(index, output);
	desktopReport();
}

void Receiver::desktopReport()
{
	if (!hooks_.desktop)
		return;
	uint8_t keyboard[8] = {}, mouse[4] = {};
	if (desktop_enabled_) {
		unsigned count = 0;
		int dx = 0, dy = 0, wheel = 0;
		for (unsigned s = 0; s < SLOT_COUNT; ++s) {
			keyboard[0] |= slots_[s].keyboard[0];
			for (unsigned i = 2; i < 8; ++i) {
				uint8_t key = slots_[s].keyboard[i];
				bool exists = !key;
				for (unsigned j = 0; j < count; ++j)
					exists |= keyboard[j + 2] == key;
				if (!exists && count < 6)
					keyboard[2 + count++] = key;
			}
			mouse[0] |= slots_[s].mouse[0];
			dx += int8_t(slots_[s].mouse[1]);
			dy += int8_t(slots_[s].mouse[2]);
			wheel += int8_t(slots_[s].mouse[3]);
			memset(slots_[s].mouse + 1, 0, 3);
		}
		int values[3] = {dx, dy, wheel};
		for (unsigned i = 0; i < 3; ++i)
			mouse[i + 1] = uint8_t(values[i] < -127 ? -127 :
					      values[i] > 127 ? 127 : values[i]);
	}
	hooks_.desktop(keyboard, mouse);
}

void Receiver::fragment(unsigned index, unsigned type, const uint8_t *body,
			unsigned size)
{
	Slot &slot = slots_[index];
	if (type == 2) {
		if (slot.fragment_drop)
			return;
		if (slot.fragment_count == 2 || slot.fragment_size + size > 54) {
			slot.fragment_count = slot.fragment_size = 0;
			slot.fragment_drop = true;
			return;
		}
		memcpy(slot.fragments + slot.fragment_size, body, size);
		slot.fragment_size += uint8_t(size);
		++slot.fragment_count;
		return;
	}
	if (!slot.fragment_drop) {
		memset(slot.reply, 0, 64);
		memcpy(slot.reply, slot.fragments, slot.fragment_size);
		unsigned count = size;
		if (count > 64u - slot.fragment_size)
			count = 64u - slot.fragment_size;
		memcpy(slot.reply + slot.fragment_size, body, count);
		slot.reply_ready = true;
		slot.feature_waiting = false;
		++slot.reply_version;
	}
	slot.fragment_count = slot.fragment_size = 0;
	slot.fragment_drop = false;
}

bool Receiver::feature(unsigned index, const uint8_t *data, unsigned size)
{
	if (index >= SLOT_COUNT || size < 2 || size > 64 || data[1] > 62 ||
	    unsigned(data[1]) + 2 > size)
		return false;
	Slot &slot = slots_[index];
	unsigned length = data[1] + 2;
	slot.last_command = data[0];
	++slot.reply_version;
	if (data[0] >= 0xf1 && data[0] <= 0xf6) {
		transferFeature(index, data, length);
		return true;
	}
	if (transfer_ == Transfer::Import &&
	    (data[0] == 0xad || data[0] == 0xb0 || data[0] == 0xaf))
		return false;
	switch (data[0]) {
	case 0xad:
		if (data[1] != 2)
			return false;
		pairing(data[2] != 0);
		return true;
	case 0xa1:
	case 0xb4:
	case 0xf0:
	case 0xf7:
		if (data[0] == 0xb4)
			stateEvent(index, !slot.connected ? 1 : slot.pending ? 3 : 2);
		return true;
	case 0xb0:
		return data[1] == 24 && storeBond(data + 2, data + 10);
	case 0xb1:
		if (data[1] != 2 || data[2] != 0)
			return false;
		desktop_enabled_ = data[3] != 0;
		desktopReport();
		return true;
	case 0xb2:
		disconnect(index);
		return true;
	case 0xb3: {
		const uint8_t commit = 5;
		if (slot.connected && slot.pending) {
			if (slot.count == 12 || !storeBond(slot.id, slot.key))
				return false;
			enqueue(index, 1, &commit, 1);
			slot.pending = false;
		}
		stateEvent(index, 2);
		return true;
	}
	case 0xaf: {
		if (data[1] != 4 || memcmp(data + 2, "wipe", 4))
			return false;
		Database replacement = db_;
		memset(replacement.bonds, 0, sizeof(replacement.bonds));
		if (!hooks_.persist(replacement)) {
			++stats_.storage_failures;
			return false;
		}
		db_ = replacement;
		committed_token_ = 0;
		for (unsigned i = 0; i < SLOT_COUNT; ++i)
			disconnect(i);
		return true;
	}
	default:
		break;
	}
	if (!slot.connected || slot.reply_ready)
		return true;
	if (slot.feature_waiting && slot.request_size == length &&
	    !memcmp(slot.request, data, length))
		return true;
	unsigned pieces = (length + MAX_PLAIN - 1) / MAX_PLAIN;
	if (slot.count + pieces > 12) {
		++stats_.queue_full;
		return false;
	}
	for (unsigned offset = 0; offset < length; offset += MAX_PLAIN) {
		unsigned count = length - offset;
		if (count > MAX_PLAIN)
			count = MAX_PLAIN;
		enqueue(index, offset + count == length ? 3 : 2,
			data + offset, count);
	}
	slot.feature_waiting = true;
	slot.request_size = uint8_t(length);
	memcpy(slot.request, data, length);
	return true;
}

void Receiver::peekFeature(unsigned index, uint8_t reply[64], uint32_t &version) const
{
	memset(reply, 0, 64);
	version = 0;
	if (index >= SLOT_COUNT)
		return;
	const Slot &slot = slots_[index];
	version = slot.reply_version;
	if (slot.reply_ready) {
		memcpy(reply, slot.reply, 64);
		return;
	}
	reply[0] = slot.last_command;
	if (slot.last_command == 0xb4) {
		reply[1] = 1;
		reply[2] = !slot.connected ? 1 : slot.pending ? 3 : 2;
	} else if (slot.last_command == 0xa1) {
		reply[1] = 34;
		memcpy(reply + 2, BUILD_TAG, 4);
		static const char hex[] = "0123456789ABCDEF";
		for (unsigned i = 0; i < 5; ++i) {
			reply[6 + i * 2] = hex[db_.address[i] >> 4];
			reply[7 + i * 2] = hex[db_.address[i] & 15];
		}
		memcpy(reply + 16, "SC2015 XIAO", 11);
	} else if (slot.last_command == 0xf0) {
		reply[1] = 62;
		reply[2] = 3;
		reply[3] = activePipes();
		reply[4] = pairing_;
		put32(reply + 5, stats_.rx);
		put32(reply + 9, stats_.authenticated);
		put32(reply + 13, stats_.mic_failures);
		put32(reply + 17, stats_.duplicates);
		put32(reply + 21, stats_.invalid);
		put32(reply + 25, stats_.queue_full);
		put32(reply + 29, stats_.storage_failures);
		// WebHID exposes no USB interface number or stable OS device path.
		reply[33] = uint8_t(index);
		reply[34] = SLOT_COUNT;
		memcpy(reply + 59, db_.address, 5);
	} else if (slot.last_command == 0xf7) {
		reply[1] = 62;
		reply[2] = 1;
	}
}

void Receiver::consumeFeature(unsigned index, uint32_t version)
{
	if (index >= SLOT_COUNT || slots_[index].reply_version != version)
		return;
	slots_[index].reply_ready = false;
	slots_[index].feature_waiting = false;
	++slots_[index].reply_version;
}

bool Receiver::takeIdentityChange()
{
	bool changed = identity_changed_;
	identity_changed_ = false;
	return changed;
}

void Receiver::transferReply(unsigned index, uint8_t command, uint8_t status,
			     unsigned payload_size)
{
	Slot &slot = slots_[index];
	memset(slot.reply, 0, sizeof(slot.reply));
	slot.reply[0] = command;
	slot.reply[1] = uint8_t(payload_size);
	slot.reply[2] = status;
	slot.reply_ready = true;
	++slot.reply_version;
}

void Receiver::finishTransfer(bool restore_pairing)
{
	bool was_import = transfer_ == Transfer::Import;
	transfer_ = Transfer::None;
	memset(&transfer_db_, 0, sizeof(transfer_db_));
	transfer_received_ = transfer_age_ = 0;
	if (was_import && restore_pairing)
		pairing(transfer_pairing_);
}

void Receiver::transferFeature(unsigned index, const uint8_t *data, unsigned size)
{
	// Local extension, not commands asserted to exist on the Valve dongle:
	// F1 snapshot, F2 record, F3 stage, F4 write, F5 commit, F6 release.
	enum { OK = 0, INVALID = 1, STALE = 2, CONNECTED = 3, BUSY = 4,
	       INCOMPLETE = 5, STORAGE = 6, DUPLICATE = 7 };
	uint8_t command = data[0];
	unsigned length = size - 2;
	const uint8_t *args = data + 2;
	if (command == 0xf1 || command == 0xf3) {
		bool importing = command == 0xf3;
		if ((!importing && length != 0) ||
		    (importing && (length != 6 || (args[0] & 7) || args[5] > BOND_COUNT))) {
			transferReply(index, command, INVALID);
			return;
		}
		if (transfer_ == Transfer::Import) {
			transferReply(index, command, BUSY);
			return;
		}
		if (importing && connectedCount()) {
			transferReply(index, command, CONNECTED);
			return;
		}
		finishTransfer(false);
		if (++transfer_token_ == 0)
			++transfer_token_;
		committed_token_ = 0;
		unsigned count = 0;
		if (importing) {
			memcpy(transfer_db_.address, args, 5);
			transfer_expected_ = args[5];
			transfer_pairing_ = pairing_;
			pairing_ = false;
			transfer_ = Transfer::Import;
			count = transfer_expected_;
		} else {
			transfer_db_ = db_;
			transfer_ = Transfer::Export;
			for (const auto &bond : db_.bonds)
				count += bond.used ? 1 : 0;
		}
		transferReply(index, command, OK, 13);
		uint8_t *reply = slots_[index].reply;
		reply[3] = 1;
		put32(reply + 4, transfer_token_);
		memcpy(reply + 8, transfer_db_.address, 5);
		reply[13] = uint8_t(count);
		reply[14] = BOND_COUNT;
		return;
	}
	unsigned expected = command == 0xf2 ? 5 : command == 0xf4 ? 29 : 4;
	if (length != expected) {
		transferReply(index, command, INVALID);
		return;
	}
	uint32_t token = get32(args);
	if (command == 0xf5 && transfer_ == Transfer::None &&
	    token == committed_token_ && token == transfer_token_ && token != 0) {
		transferReply(index, command, OK, 5);
		put32(slots_[index].reply + 3, token);
		return;
	}
	if (!token || token != transfer_token_ || transfer_ == Transfer::None) {
		transferReply(index, command, STALE);
		return;
	}
	transfer_age_ = 0;
	if (command == 0xf6) {
		finishTransfer(true);
		transferReply(index, command, OK, 5);
		put32(slots_[index].reply + 3, token);
		return;
	}
	if (command == 0xf2) {
		if (transfer_ != Transfer::Export || args[4] >= BOND_COUNT) {
			transferReply(index, command, INVALID);
			return;
		}
		const Bond &bond = transfer_db_.bonds[args[4]];
		transferReply(index, command, OK, 31);
		uint8_t *reply = slots_[index].reply;
		reply[3] = args[4];
		reply[4] = bond.used;
		if (bond.used) {
			memcpy(reply + 5, bond.id, 8);
			memcpy(reply + 13, bond.key, 16);
		}
		put32(reply + 29, token);
		return;
	}
	if (transfer_ != Transfer::Import) {
		transferReply(index, command, INVALID);
		return;
	}
	if (command == 0xf4) {
		unsigned record = args[4];
		if (record >= transfer_expected_) {
			transferReply(index, command, INVALID);
			return;
		}
		for (unsigned i = 0; i < transfer_expected_; ++i)
			if (i != record && transfer_db_.bonds[i].used &&
			    !memcmp(transfer_db_.bonds[i].id, args + 5, 8)) {
				transferReply(index, command, DUPLICATE);
				return;
			}
		Bond &bond = transfer_db_.bonds[record];
		bond.used = true;
		memcpy(bond.id, args + 5, 8);
		memcpy(bond.key, args + 13, 16);
		transfer_received_ |= 1u << record;
		transferReply(index, command, OK, 6);
		slots_[index].reply[3] = uint8_t(record);
		put32(slots_[index].reply + 4, token);
		return;
	}
	if (transfer_received_ != ((1u << transfer_expected_) - 1)) {
		transferReply(index, command, INCOMPLETE);
		return;
	}
	if (connectedCount()) {
		transferReply(index, command, CONNECTED);
		return;
	}
	if (!hooks_.persist(transfer_db_)) {
		++stats_.storage_failures;
		transferReply(index, command, STORAGE);
		return;
	}
	identity_changed_ |= memcmp(db_.address, transfer_db_.address, 5) != 0;
	db_ = transfer_db_;
	finishTransfer(false);
	committed_token_ = token;
	refreshChallenge();
	bool has_bond = false;
	for (const auto &bond : db_.bonds)
		has_bond |= bond.used;
	pairing(!has_bond);
	transferReply(index, command, OK, 5);
	put32(slots_[index].reply + 3, token);
}

} // namespace sc
