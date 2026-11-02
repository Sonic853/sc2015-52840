#include "radio.h"
#include "radio_policy.h"
#include <Arduino.h>
#include <nrf.h>
#include <string.h>

namespace radio {

// No SoftDevice is enabled. TIMER3 and PPI 0/1 are exclusively owned here.
static constexpr uint32_t PPI_CAPTURE = 1u << 0;
static constexpr uint32_t PPI_ACK = 1u << 1;
static constexpr uint32_t HOP_US = 1500;
static constexpr uint32_t ACK_TURNAROUND_US = 130;
static constexpr uint32_t FAST_RAMP_US = 40;

struct alignas(4) DmaPacket {
	uint8_t size, s1, payload[32];
};
static_assert(offsetof(DmaPacket, s1) == 1 && offsetof(DmaPacket, payload) == 2,
	      "ESB DMA layout must be length, S1, payload");

static DmaPacket rx, tx;
static ResponseSnapshot responses[2];
static volatile uint8_t active_response;
static Received queue[32];
static volatile uint8_t head, tail;
static uint8_t channels[7];
static volatile uint8_t channel_index, pipe_mask;
static volatile uint8_t current_channel;
static volatile uint32_t hop_count;
static volatile uint32_t ack_rx_end;
static volatile bool sending_ack, hop_pending, running;
static Statistics counters{};

class Critical {
	uint32_t previous;
public:
	Critical() : previous(__get_PRIMASK()) { __disable_irq(); }
	~Critical() { if (!previous) __enable_irq(); }
};

uint32_t now()
{
	NRF_TIMER3->TASKS_CAPTURE[4] = 1;
	return NRF_TIMER3->CC[4];
}

static uint32_t irqNow()
{
	NRF_TIMER3->TASKS_CAPTURE[5] = 1;
	return NRF_TIMER3->CC[5];
}

static bool disabled()
{
	if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled)
		return true;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	uint32_t start = irqNow();
	while (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
		if (uint32_t(irqNow() - start) > 50) {
			++counters.faults;
			return false;
		}
	}
	return true;
}

static void receiveStart()
{
	NRF_RADIO->EVENTS_READY = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->PACKETPTR = uint32_t(&rx);
	NRF_RADIO->RXADDRESSES = pipe_mask;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_DISABLE_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
	NRF_RADIO->TASKS_RXEN = 1;
}

static void applyChannel()
{
	uint8_t channel = channels[channel_index];
	current_channel = channel;
	NRF_RADIO->FREQUENCY = channel;
	// nRF52840 has no -6 dBm step; use the next lower supported edge power.
	NRF_RADIO->TXPOWER = channel <= 3 || channel >= 80 ?
		RADIO_TXPOWER_TXPOWER_Neg8dBm : RADIO_TXPOWER_TXPOWER_0dBm;
	hop_pending = false;
}

void irqEnd()
{
	if (!running)
		return;
	if (NRF_RADIO->EVENTS_READY) {
		NRF_RADIO->EVENTS_READY = 0;
		if (sending_ack) {
			++counters.ack_ready;
			uint32_t delay = irqNow() - ack_rx_end;
			counters.last_ready_delay = uint16_t(delay > 65535 ? 65535 : delay);
		}
	}
	if (!NRF_RADIO->EVENTS_END)
		return;
	NRF_RADIO->EVENTS_END = 0;
	if (sending_ack) {
		++counters.ack_completed;
		NRF_PPI->CHENCLR = PPI_ACK;
		sending_ack = false;
		if (!disabled())
			return;
		if (hop_pending)
			applyChannel();
		receiveStart();
		return;
	}
	uint32_t end_time = NRF_TIMER3->CC[0];
	unsigned pipe = NRF_RADIO->RXMATCH;
	bool valid = NRF_RADIO->CRCSTATUS && rx.size && rx.size <= 32 && pipe < 6;
	bool retained = false;
	if (valid) {
		++counters.crc_ok;
		++counters.pipe_rx[pipe];
		counters.last_pipe = uint8_t(pipe);
		counters.last_channel = current_channel;
		counters.last_size = rx.size;
		counters.last_s1 = rx.s1;
		counters.last_header = rx.payload[0];
		uint32_t latency = irqNow() - end_time;
		if (latency > counters.max_irq_delay)
			counters.max_irq_delay = uint16_t(latency > 65535 ? 65535 : latency);
		uint8_t next = (head + 1) % 32;
		if (next == tail) {
			++counters.queue_full;
		} else {
			Received &item = queue[head];
			item.packet.size = rx.size;
			memcpy(item.packet.data, rx.payload, rx.size);
			item.pipe = uint8_t(pipe);
			item.channel = current_channel;
			item.rssi = uint8_t(NRF_RADIO->RSSISAMPLE);
			item.timestamp = end_time;
			item.hop = hop_count;
			__DMB();
			head = next;
			retained = true;
		}
	} else {
		++counters.crc_bad;
	}
	if (!disabled())
		return;
	if (shouldAck(valid, retained, rx.s1)) {
		const ResponseSnapshot &snapshot = responses[active_response];
		const sc::Packet &ack = snapshot.select(pipe, (rx.payload[0] >> 2) & 1,
					       rx.payload, rx.size);
		tx.size = ack.size;
		tx.s1 = rx.s1;
		++counters.ack_scheduled;
		if (!ack.size)
			++counters.ack_empty;
		counters.last_ack_size = ack.size;
		counters.last_tx_pipe = uint8_t(pipe);
		ack_rx_end = end_time;
		if (ack.size)
			memcpy(tx.payload, ack.data, ack.size);
		NRF_RADIO->PACKETPTR = uint32_t(&tx);
		NRF_RADIO->TXADDRESS = pipe;
		NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
				    RADIO_SHORTS_END_DISABLE_Msk;
		NRF_RADIO->EVENTS_END = 0;
		NRF_TIMER3->EVENTS_COMPARE[1] = 0;
		uint32_t deadline = end_time + ACK_TURNAROUND_US - FAST_RAMP_US;
		NRF_TIMER3->CC[1] = deadline;
		sending_ack = true;
		__DMB();
		// Fast ramp-up leaves time to choose an already serialized ACK. Crypto
		// never runs here. Late deadlines send immediately instead of waiting
		// for a 32-bit timer wrap.
		if (int32_t(deadline - irqNow()) > 4)
			NRF_PPI->CHENSET = PPI_ACK;
		else {
			++counters.late_ack;
			NRF_RADIO->TASKS_TXEN = 1;
		}
		return;
	}
	// Overflow is counted by queue_full; ack_skipped retains its no-ACK meaning.
	if (valid && !wantsAck(valid, rx.s1))
		++counters.ack_skipped;
	if (hop_pending)
		applyChannel();
	receiveStart();
}

void irqTimer()
{
	if (!running || !NRF_TIMER3->EVENTS_COMPARE[2])
		return;
	NRF_TIMER3->EVENTS_COMPARE[2] = 0;
	uint32_t deadline = NRF_TIMER3->CC[2];
	uint8_t index = channel_index;
	hop_count += sc::advanceHops(irqNow(), deadline, index);
	channel_index = index;
	NRF_TIMER3->CC[2] = deadline;
	hop_pending = true;
	// Retuning and END handling share PACKETPTR/STATE. Keep the higher
	// priority RADIO handler from starting an ACK between the test and the
	// retune. A packet that has an address or END already pending owns the
	// old channel until its END/ACK is handled.
	NVIC_DisableIRQ(RADIO_IRQn);
	if (canRetune(sending_ack, NRF_RADIO->EVENTS_END != 0,
		      NRF_RADIO->EVENTS_ADDRESS != 0) && disabled()) {
		applyChannel();
		receiveStart();
	}
	NVIC_EnableIRQ(RADIO_IRQn);
}

bool begin(const uint8_t address[5])
{
	head = tail = channel_index = 0;
	hop_count = 0;
	sending_ack = hop_pending = false;
	NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
	NRF_CLOCK->TASKS_HFCLKSTART = 1;
	uint32_t start = micros();
	while (!NRF_CLOCK->EVENTS_HFCLKSTARTED &&
	       !(NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk))
		if (uint32_t(micros() - start) > 100000)
			return false;
	NRF_TIMER3->TASKS_STOP = 1;
	NRF_TIMER3->INTENCLR = 0xffffffff;
	NRF_TIMER3->EVENTS_COMPARE[1] = 0;
	NRF_TIMER3->EVENTS_COMPARE[2] = 0;
	NRF_TIMER3->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER3->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
	NRF_TIMER3->PRESCALER = 4;
	NRF_TIMER3->TASKS_CLEAR = 1;
	NRF_TIMER3->TASKS_START = 1;
	if (!disabled())
		return false;
	sc::makeHops(address, channels);
	NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_1Mbit;
	NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast;
	NRF_RADIO->PCNF0 = 0x00030006;
	NRF_RADIO->PCNF1 = 0x01040020;
	NRF_RADIO->CRCCNF = 2;
	NRF_RADIO->CRCPOLY = 0x11021;
	NRF_RADIO->CRCINIT = 0xffff;
	NRF_RADIO->BASE0 = sc::radioBase(sc::DISCOVERY_ADDRESS);
	NRF_RADIO->BASE1 = sc::radioBase(address);
	uint32_t prefixes = sc::reverse8(sc::DISCOVERY_ADDRESS[0]);
	prefixes |= uint32_t(sc::reverse8(address[0])) << 8;
	prefixes |= uint32_t(sc::reverse8(address[0] | 2)) << 16;
	prefixes |= uint32_t(sc::reverse8(address[0] | 3)) << 24;
	NRF_RADIO->PREFIX0 = prefixes;
	NRF_RADIO->PREFIX1 = sc::reverse8(address[0] | 4) |
			     (uint32_t(sc::reverse8(address[0] | 5)) << 8);
	NRF_RADIO->TIFS = ACK_TURNAROUND_US;
	NRF_RADIO->INTENSET = RADIO_INTENSET_END_Msk | RADIO_INTENSET_READY_Msk;
	NRF_PPI->CH[0].EEP = uint32_t(&NRF_RADIO->EVENTS_END);
	NRF_PPI->CH[0].TEP = uint32_t(&NRF_TIMER3->TASKS_CAPTURE[0]);
	NRF_PPI->CH[1].EEP = uint32_t(&NRF_TIMER3->EVENTS_COMPARE[1]);
	NRF_PPI->CH[1].TEP = uint32_t(&NRF_RADIO->TASKS_TXEN);
	NRF_PPI->CHENSET = PPI_CAPTURE;
	NRF_TIMER3->CC[2] = now() + HOP_US;
	NRF_TIMER3->INTENSET = TIMER_INTENSET_COMPARE2_Msk;
	NVIC_SetPriority(RADIO_IRQn, 1);
	NVIC_SetPriority(TIMER3_IRQn, 2);
	NVIC_ClearPendingIRQ(RADIO_IRQn);
	NVIC_ClearPendingIRQ(TIMER3_IRQn);
	running = true;
	NVIC_EnableIRQ(RADIO_IRQn);
	NVIC_EnableIRQ(TIMER3_IRQn);
	applyChannel();
	receiveStart();
	return true;
}

void publish(const sc::Receiver &receiver)
{
	// Build a complete epoch without masking interrupts, then swap one byte.
	// The ISR can never mix a refreshed challenge with old replay/pipe data.
	uint8_t next = active_response ^ 1;
	ResponseSnapshot &snapshot = responses[next];
	for (unsigned pipe = 0; pipe < 6; ++pipe)
		for (unsigned parity = 0; parity < 2; ++parity)
			snapshot.acknowledgements[pipe][parity] = receiver.acknowledgement(pipe, parity);
	for (unsigned i = 0; i < sc::SLOT_COUNT; ++i)
		snapshot.joins[i] = receiver.joinReplay(i);
	snapshot.mask = receiver.activePipes();
	Critical critical;
	__DMB();
	active_response = next;
	pipe_mask = snapshot.mask;
	NRF_RADIO->RXADDRESSES = pipe_mask;
}

bool pop(Received &received)
{
	if (tail == head)
		return false;
	received = queue[tail];
	__DMB();
	tail = (tail + 1) % 32;
	return true;
}

bool idleHops(uint32_t &absolute_hop)
{
	Critical critical;
	if (tail != head)
		return false;
	absolute_hop = hop_count;
	return true;
}

void pause()
{
	NVIC_DisableIRQ(RADIO_IRQn);
	NVIC_DisableIRQ(TIMER3_IRQn);
	NRF_PPI->CHENCLR = PPI_CAPTURE | PPI_ACK;
	running = false;
	disabled();
	sending_ack = false;
}

void resume()
{
	// NVMC stalls instruction fetch while the peer keeps hopping. Preserve
	// the original absolute schedule instead of restarting its phase here.
	uint32_t deadline = NRF_TIMER3->CC[2];
	uint8_t index = channel_index;
	hop_count += sc::advanceHops(now(), deadline, index);
	channel_index = index;
	NRF_TIMER3->EVENTS_COMPARE[2] = 0;
	NRF_TIMER3->CC[2] = deadline;
	NRF_PPI->CHENSET = PPI_CAPTURE;
	applyChannel();
	NVIC_ClearPendingIRQ(RADIO_IRQn);
	NVIC_ClearPendingIRQ(TIMER3_IRQn);
	running = true;
	NVIC_EnableIRQ(RADIO_IRQn);
	NVIC_EnableIRQ(TIMER3_IRQn);
	receiveStart();
}

const Statistics &statistics() { return counters; }

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		bytes[i] = uint8_t(value >> (8 * i));
}

void pairingDiagnostics(uint8_t report[64])
{
	Critical critical;
	memset(report, 0, 64);
	report[0] = 0xf7;
	report[1] = 62;
	report[2] = 1;
	report[3] = FIRMWARE_REVISION;
	report[4] = counters.last_pipe;
	report[5] = counters.last_channel;
	report[6] = counters.last_size;
	report[7] = counters.last_s1;
	report[8] = counters.last_header;
	report[9] = counters.last_ack_size;
	report[10] = counters.last_tx_pipe;
	report[11] = uint8_t(NRF_CLOCK->HFCLKSTAT);
	for (unsigned i = 0; i < 6; ++i)
		put32(report + 12 + 4 * i, counters.pipe_rx[i]);
	put32(report + 36, counters.ack_scheduled);
	put32(report + 40, counters.ack_ready);
	put32(report + 44, counters.ack_completed);
	put32(report + 48, counters.ack_skipped);
	put32(report + 52, counters.ack_empty);
	report[56] = uint8_t(counters.max_irq_delay);
	report[57] = uint8_t(counters.max_irq_delay >> 8);
	report[58] = uint8_t(counters.last_ready_delay);
	report[59] = uint8_t(counters.last_ready_delay >> 8);
	report[60] = uint8_t(NRF_RADIO->STATE);
	report[61] = current_channel;
	report[62] = uint8_t(NRF_PPI->CHEN);
	report[63] = FIRMWARE_REVISION;
}

bool random(uint8_t *data, size_t size)
{
	NRF_RNG->CONFIG = 1;
	NRF_RNG->TASKS_START = 1;
	uint32_t start = micros();
	for (size_t i = 0; i < size; ++i) {
		NRF_RNG->EVENTS_VALRDY = 0;
		while (!NRF_RNG->EVENTS_VALRDY)
			if (uint32_t(micros() - start) > 100000) {
				NRF_RNG->TASKS_STOP = 1;
				return false;
			}
		data[i] = uint8_t(NRF_RNG->VALUE);
	}
	NRF_RNG->TASKS_STOP = 1;
	return true;
}

bool aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	struct alignas(4) Ecb {
		uint8_t key[16], input[16], output[16];
	} ecb;
	memcpy(ecb.key, key, 16);
	memcpy(ecb.input, in, 16);
	NRF_ECB->EVENTS_ENDECB = 0;
	NRF_ECB->EVENTS_ERRORECB = 0;
	NRF_ECB->ECBDATAPTR = uint32_t(&ecb);
	NRF_ECB->TASKS_STARTECB = 1;
	uint32_t start = micros();
	while (!NRF_ECB->EVENTS_ENDECB) {
		if (NRF_ECB->EVENTS_ERRORECB || uint32_t(micros() - start) > 500) {
			NRF_ECB->TASKS_STOPECB = 1;
			return false;
		}
	}
	memcpy(out, ecb.output, 16);
	memset(&ecb, 0, sizeof(ecb));
	return true;
}

} // namespace radio

extern "C" void RADIO_IRQHandler(void) { radio::irqEnd(); }
extern "C" void TIMER3_IRQHandler(void) { radio::irqTimer(); }
