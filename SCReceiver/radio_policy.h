#pragma once
#include <stdint.h>
#include "protocol.h"
#include <string.h>

namespace radio {

constexpr uint8_t FIRMWARE_REVISION = 18;

// Diagnostic time is a full 32-bit, 1 MHz TIMER3 count. Preserve its epoch
// when an identity change restarts the RF timer; unsigned elapsed arithmetic
// then works across both restarts and the natural ~71.6 minute wrap.
struct QueueClock {
	uint32_t offset = 0;
	bool started = false;

	uint32_t sample(uint32_t raw, uint32_t boot_us) const
	{
		return started ? offset + raw : boot_us;
	}
	void restart(uint32_t raw, uint32_t boot_us)
	{
		offset = sample(raw, boot_us);
		started = true;
	}
};

// nRF5 ESB DMA S1 is (PID << 1) | ACK_REQUESTED. Target instructions
// P:3306..3316 write 1 for the normal ACK transaction; treating bit zero
// as an active-high no-ACK flag suppresses every normal discovery reply.
constexpr bool wantsAck(bool crc_valid, uint8_t s1)
{
	return crc_valid && (s1 & 1u) != 0;
}

// A payload ACK can advance the peer before deferred RX authentication runs.
// Retain that RX first: dropping its ACK phase can strand a pending lane.
constexpr bool shouldAck(bool valid, bool retained, uint8_t s1)
{
	return retained && wantsAck(valid, s1);
}

constexpr bool canRetune(bool ack_active, bool end_pending, bool address_seen)
{
	return !ack_active && !end_pending && !address_seen;
}

struct ResponseSnapshot {
	sc::Packet acknowledgements[6][2];
	sc::JoinReplay joins[sc::SLOT_COUNT];
	uint8_t deferred_mask[6];
	uint8_t mask;

	// The driver validates pipe<6 and size<=32 before selecting a reply.
	const sc::Packet &select(unsigned pipe, unsigned parity,
				const uint8_t *payload, unsigned size) const
	{
		if (pipe == 1) {
			for (const sc::JoinReplay &join : joins)
				if (join.active && join.request.size == size &&
				    !memcmp(join.request.data, payload, size))
					return join.acknowledgement;
		}
		return acknowledgements[pipe][parity & 1];
	}
};

} // namespace radio
