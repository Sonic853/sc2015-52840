#include "storage.h"
#include "radio.h"
#include <Arduino.h>
#include <nrf.h>
#include <string.h>

namespace storage {

static uint32_t active_page, generation;
static bool radio_running;

void radioRunning(bool running) { radio_running = running; }

bool load(sc::Database &database)
{
	sc::Database a{}, b{};
	uint32_t ga = 0, gb = 0;
	bool va = sc::decodeDatabase(reinterpret_cast<const uint8_t *>(PAGE_A), a, ga);
	bool vb = sc::decodeDatabase(reinterpret_cast<const uint8_t *>(PAGE_B), b, gb);
	if (va && (!vb || !sc::generationAfter(gb, ga))) {
		database = a;
		generation = ga;
		active_page = PAGE_A;
		return true;
	}
	if (vb) {
		database = b;
		generation = gb;
		active_page = PAGE_B;
		return true;
	}
	memset(&database, 0, sizeof(database));
	uint32_t id0 = NRF_FICR->DEVICEID[0], id1 = NRF_FICR->DEVICEID[1];
	for (unsigned i = 0; i < 4; ++i)
		database.address[i + 1] = uint8_t(id0 >> (8 * i));
	database.address[0] = uint8_t(id1) & 0xf8;
	// Avoid long runs of identical bits in an otherwise erased FICR identity.
	for (unsigned i = 1; i < 5; ++i)
		if (!database.address[i] || database.address[i] == 0xff)
			database.address[i] ^= uint8_t(0x51 + i);
	return save(database);
}

static bool ready(uint32_t timeout)
{
	uint32_t start = micros();
	while (!NRF_NVMC->READY)
		if (uint32_t(micros() - start) > timeout)
			return false;
	return true;
}

static bool program(uint32_t page, const uint8_t bytes[sc::STORE_SIZE])
{
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
	if (!ready(100000))
		return false;
	NRF_NVMC->ERASEPAGE = page;
	if (!ready(100000))
		return false;
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
	if (!ready(100000))
		return false;
	for (unsigned offset = 0; offset < sc::STORE_SIZE - 4; offset += 4) {
		uint32_t word;
		memcpy(&word, bytes + offset, sizeof(word));
		*reinterpret_cast<volatile uint32_t *>(page + offset) = word;
		if (!ready(100000))
			return false;
	}
	if (memcmp(reinterpret_cast<const void *>(page), bytes, sc::STORE_SIZE - 4))
		return false;
	*reinterpret_cast<volatile uint32_t *>(page + sc::STORE_SIZE - 4) = 0;
	return ready(100000);
}

bool save(const sc::Database &database)
{
	alignas(4) uint8_t bytes[sc::STORE_SIZE];
	if (!sc::encodeDatabase(database, generation + 1, bytes))
		return false;
	uint32_t next = active_page == PAGE_A ? PAGE_B : PAGE_A;
	if (radio_running)
		radio::pause();
	bool success = program(next, bytes);
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
	success = ready(100000) && success;
	__DSB();
	__ISB();
	if (radio_running)
		radio::resume();
	sc::Database verify{};
	uint32_t read_generation;
	if (success)
		success = sc::decodeDatabase(reinterpret_cast<const uint8_t *>(next),
					     verify, read_generation) &&
			  read_generation == generation + 1;
	if (success) {
		active_page = next;
		++generation;
	}
	memset(bytes, 0, sizeof(bytes));
	return success;
}

} // namespace storage
