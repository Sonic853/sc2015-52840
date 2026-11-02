#pragma once

#include "protocol.h"

namespace radio {

struct Received {
	sc::Packet packet;
	uint32_t timestamp;
	uint8_t pipe, channel, rssi;
};

struct Statistics {
	volatile uint32_t crc_ok, crc_bad, queue_full, late_ack, faults;
};

bool begin(const uint8_t address[5]);
void setPipes(uint8_t mask);
void setAck(unsigned pipe, unsigned peer_parity, const sc::Packet &packet);
bool pop(Received &received);
uint32_t takeHops();
void pause();
void resume();
const Statistics &statistics();
uint32_t now();
bool random(uint8_t *data, size_t size);
bool aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

} // namespace radio
