#include "protocol.h"
#include <string.h>

namespace sc {

const uint8_t DISCOVERY_ADDRESS[5] = {0x56, 0x41, 0x4c, 0x56, 0x45};
const uint8_t BOOTSTRAP_KEY[16] = {
	0x2e, 0x68, 0xb1, 0x0f, 0x87, 0xef, 0x24, 0x08,
	0x40, 0x3b, 0x8f, 0x7c, 0x48, 0x17, 0x4e, 0x37,
};

void nonce(uint8_t out[13], uint64_t counter, bool downlink,
	   const uint8_t iv[8])
{
	for (unsigned i = 0; i < 5; ++i)
		out[i] = uint8_t(counter >> (8 * i));
	out[4] = (out[4] & 0x7f) | (downlink ? 0x80 : 0);
	memcpy(out + 5, iv, 8);
}

static bool mac(AesBlock aes, const uint8_t key[16], const uint8_t n[13],
		uint8_t header, const uint8_t *plain, unsigned size,
		uint8_t tag[4])
{
	uint8_t block[16] = {0x49};
	uint8_t chain[16];
	memcpy(block + 1, n, 13);
	block[15] = uint8_t(size);
	if (!aes(key, block, chain))
		return false;
	memset(block, 0, sizeof(block));
	block[1] = 1;
	block[2] = header & 0x83;
	for (unsigned i = 0; i < 16; ++i)
		block[i] ^= chain[i];
	if (!aes(key, block, chain))
		return false;
	for (unsigned offset = 0; offset < size; offset += 16) {
		unsigned count = size - offset;
		if (count > 16)
			count = 16;
		memcpy(block, chain, 16);
		for (unsigned i = 0; i < count; ++i)
			block[i] ^= plain[offset + i];
		if (!aes(key, block, chain))
			return false;
	}
	block[0] = 1;
	memcpy(block + 1, n, 13);
	block[14] = block[15] = 0;
	uint8_t stream[16];
	if (!aes(key, block, stream))
		return false;
	for (unsigned i = 0; i < 4; ++i)
		tag[i] = chain[i] ^ stream[i];
	return true;
}

static bool ctr(AesBlock aes, const uint8_t key[16], const uint8_t n[13],
		const uint8_t *input, uint8_t *output, unsigned size)
{
	uint8_t block[16] = {1};
	uint8_t stream[16];
	memcpy(block + 1, n, 13);
	for (unsigned offset = 0; offset < size; offset += 16) {
		block[15] = uint8_t(offset / 16 + 1);
		if (!aes(key, block, stream))
			return false;
		unsigned count = size - offset;
		if (count > 16)
			count = 16;
		for (unsigned i = 0; i < count; ++i)
			output[offset + i] = input[offset + i] ^ stream[i];
	}
	return true;
}

bool seal(AesBlock aes, const uint8_t key[16], const uint8_t iv[8],
	  uint64_t counter, bool downlink, uint8_t header,
	  const uint8_t *plain, unsigned size, Packet &wire)
{
	wire.size = 0;
	if (!aes || !plain || !size || size > MAX_PLAIN || counter > COUNTER_MASK)
		return false;
	uint8_t n[13], tag[4];
	nonce(n, counter, downlink, iv);
	if (!mac(aes, key, n, header, plain, size, tag) ||
	    !ctr(aes, key, n, plain, wire.data + 1, size))
		return false;
	wire.data[0] = header;
	memcpy(wire.data + 1 + size, tag, 4);
	wire.size = uint8_t(size + 5);
	return true;
}

bool open(AesBlock aes, const uint8_t key[16], const uint8_t iv[8],
	  uint64_t counter, bool downlink, const Packet &wire,
	  uint8_t plain[MAX_PLAIN], unsigned &size)
{
	size = 0;
	if (!aes || wire.size < 6 || wire.size > 32 || counter > COUNTER_MASK)
		return false;
	unsigned count = wire.size - 5;
	uint8_t n[13], candidate[MAX_PLAIN] = {}, tag[4];
	nonce(n, counter, downlink, iv);
	if (!ctr(aes, key, n, wire.data + 1, candidate, count) ||
	    !mac(aes, key, n, wire.data[0], candidate, count, tag))
		return false;
	uint8_t difference = 0;
	for (unsigned i = 0; i < 4; ++i)
		difference |= tag[i] ^ wire.data[1 + count + i];
	if (difference)
		return false;
	memcpy(plain, candidate, count);
	size = count;
	return true;
}

uint64_t uplinkCounter(uint64_t previous, uint8_t header)
{
	return (previous + (((header >> 4) - unsigned(previous & 7)) & 7)) &
	       COUNTER_MASK;
}

uint8_t reverse8(uint8_t value)
{
	value = ((value & 0x55) << 1) | ((value >> 1) & 0x55);
	value = ((value & 0x33) << 2) | ((value >> 2) & 0x33);
	return uint8_t((value << 4) | (value >> 4));
}

uint32_t radioBase(const uint8_t address[5])
{
	uint32_t result = 0;
	for (unsigned i = 1; i < 5; ++i)
		result = (result << 8) | reverse8(address[i]);
	return result;
}

static uint32_t advance(uint32_t &state)
{
	state = (state >> 1) ^ ((0u - (state & 1)) & 0xd0000001u);
	return state;
}

static void discardThree(uint32_t &state)
{
	for (unsigned i = 0; i < 3; ++i)
		advance(state);
}

void makeHops(const uint8_t address[5], uint8_t hops[7])
{
	uint32_t state = (uint32_t(address[1]) << 24) |
			 (uint32_t(address[2]) << 16) |
			 (uint32_t(address[3]) << 8) | address[4];
	state ^= address[0] & 0xf8;
	if (!state)
		state = 1;
	for (unsigned i = 0; i < 7; ++i) {
		hops[i] = uint8_t(advance(state) % 11 + 11 * i + 2);
		discardThree(state);
	}
	for (unsigned i = 1; i < 7; ++i)
		if (int(hops[i]) - int(hops[i - 1]) < 5)
			hops[i] = hops[i - 1] + 5;
	if (hops[6] > 80)
		hops[6] = 80;
	for (unsigned i = 0; i < 7; ++i) {
		if (hops[i] < 3)
			hops[i] = 3;
		if (hops[i] > 80)
			hops[i] = 80;
	}
	for (unsigned i = 6; i > 1; --i) {
		unsigned other = advance(state) % (i + 1);
		uint8_t temporary = hops[i];
		hops[i] = hops[other];
		hops[other] = temporary;
		discardThree(state);
	}
}

uint32_t crc32(const uint8_t *data, size_t size)
{
	uint32_t crc = 0xffffffff;
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0xedb88320);
	}
	return ~crc;
}

uint32_t advanceHops(uint32_t now, uint32_t &deadline, uint8_t &index)
{
	if (int32_t(now - deadline) < 0)
		return 0;
	uint32_t count = uint32_t(now - deadline) / 1500 + 1;
	deadline += count * 1500;
	index = uint8_t((index + count) % 7);
	return count;
}

static void put32(uint8_t *out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out[i] = uint8_t(value >> (i * 8));
}

static uint32_t get32(const uint8_t *in)
{
	return uint32_t(in[0]) | (uint32_t(in[1]) << 8) |
	       (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}

bool encodeDatabase(const Database &db, uint32_t generation,
		    uint8_t out[STORE_SIZE])
{
	if (db.address[0] & 7)
		return false;
	memset(out, 0, STORE_SIZE);
	memcpy(out, "SC15", 4);
	put32(out + 4, 1);
	put32(out + 8, generation);
	memcpy(out + 12, db.address, 5);
	for (unsigned i = 0; i < BOND_COUNT; ++i) {
		uint8_t *record = out + 20 + 25 * i;
		record[0] = db.bonds[i].used ? 0xf0 : 0;
		if (db.bonds[i].used) {
			memcpy(record + 1, db.bonds[i].id, 8);
			memcpy(record + 9, db.bonds[i].key, 16);
		}
	}
	put32(out + 520, crc32(out, 520));
	// out[524:528] is the committed word; the writer omits it until last.
	return true;
}

bool decodeDatabase(const uint8_t in[STORE_SIZE], Database &db,
		    uint32_t &generation)
{
	if (memcmp(in, "SC15", 4) || get32(in + 4) != 1 ||
	    get32(in + 524) != 0 || get32(in + 520) != crc32(in, 520) ||
	    (in[12] & 7))
		return false;
	Database candidate{};
	memcpy(candidate.address, in + 12, 5);
	for (unsigned i = 0; i < BOND_COUNT; ++i) {
		const uint8_t *record = in + 20 + 25 * i;
		if (record[0] != 0 && record[0] != 0xf0)
			return false;
		candidate.bonds[i].used = record[0] == 0xf0;
		memcpy(candidate.bonds[i].id, record + 1, 8);
		memcpy(candidate.bonds[i].key, record + 9, 16);
	}
	db = candidate;
	generation = get32(in + 8);
	return true;
}

bool generationAfter(uint32_t a, uint32_t b)
{
	return a != b && uint32_t(a - b) < 0x80000000u;
}

} // namespace sc
