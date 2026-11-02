#pragma once
#include <stdint.h>

namespace radio {

constexpr uint8_t FIRMWARE_REVISION = 3;

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

} // namespace radio
