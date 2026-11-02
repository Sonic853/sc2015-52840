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
	volatile uint32_t pipe_rx[6];
	volatile uint32_t ack_scheduled, ack_ready, ack_completed, ack_skipped, ack_empty;
	volatile uint16_t max_irq_delay, last_ready_delay;
	volatile uint8_t last_pipe, last_channel, last_size, last_s1, last_header;
	volatile uint8_t last_ack_size, last_tx_pipe;
};

bool begin(const uint8_t address[5]);
void setPipes(uint8_t mask);
void setAck(unsigned pipe, unsigned peer_parity, const sc::Packet &packet);
bool pop(Received &received);
uint32_t takeHops();
void pause();
void resume();
const Statistics &statistics();
void pairingDiagnostics(uint8_t report[64]);
uint32_t now();
bool random(uint8_t *data, size_t size);
bool aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

} // namespace radio
