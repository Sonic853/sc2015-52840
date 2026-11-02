#pragma once
#include <stdint.h>
#include "protocol.h"
#include <string.h>

namespace radio {

constexpr uint8_t FIRMWARE_REVISION = 5;

// nRF5 ESB DMA S1 is (PID << 1) | ACK_REQUESTED. Target instructions
// P:3306..3316 write 1 for the normal ACK transaction; treating bit zero
// as an active-high no-ACK flag suppresses every normal discovery reply.
constexpr bool wantsAck(bool crc_valid, uint8_t s1)
{
	return crc_valid && (s1 & 1u) != 0;
}

constexpr bool canRetune(bool ack_active, bool end_pending, bool address_seen)
{
	return !ack_active && !end_pending && !address_seen;
}

struct ResponseSnapshot {
	sc::Packet acknowledgements[6][2];
	sc::JoinReplay joins[sc::SLOT_COUNT];
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
