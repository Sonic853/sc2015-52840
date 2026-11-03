#include "storage.h"
#include "radio.h"
#include <Arduino.h>
#include <nrf.h>
#include <string.h>

namespace storage {

static uint32_t active_page, generation;
static uint32_t config_page, config_generation;
static bool radio_running;
static input_mode::Config saved_config;
static input_mode::BootState saved_boot;
static input_mode::Mode running_mode;
static bool saveSettings(const input_mode::Config &, input_mode::BootState);

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
	uint32_t start = radio::queueMicros();
	while (!NRF_NVMC->READY)
		if (uint32_t(radio::queueMicros() - start) > timeout)
			return false;
	return true;
}

static bool program(uint32_t page, const uint8_t *bytes, unsigned size = sc::STORE_SIZE)
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
	for (unsigned offset = 0; offset < size - 4; offset += 4) {
		uint32_t word;
		memcpy(&word, bytes + offset, sizeof(word));
		*reinterpret_cast<volatile uint32_t *>(page + offset) = word;
		if (!ready(100000))
			return false;
	}
	if (memcmp(reinterpret_cast<const void *>(page), bytes, size - 4))
		return false;
	*reinterpret_cast<volatile uint32_t *>(page + size - 4) = 0;
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

bool loadInputConfig(input_mode::Config &config, input_mode::Mode &boot_mode)
{
	input_mode::Config a{}, b{};
	input_mode::BootState ba = input_mode::defaultBootState(), bb = ba;
	uint32_t ga = 0, gb = 0;
	bool va = input_mode::decodeRecord(reinterpret_cast<const uint8_t *>(CONFIG_A), a, ga, &ba);
	bool vb = input_mode::decodeRecord(reinterpret_cast<const uint8_t *>(CONFIG_B), b, gb, &bb);
	if (va && (!vb || !sc::generationAfter(gb, ga))) {
		config = a; saved_boot = ba; config_page = CONFIG_A; config_generation = ga;
	} else if (vb) {
		config = b; saved_boot = bb; config_page = CONFIG_B; config_generation = gb;
	} else {
		config = input_mode::defaults(); config_page = 0; config_generation = 0;
		saved_boot = input_mode::defaultBootState();
	}
	saved_config = config;
	input_mode::BootState consumed = saved_boot;
	boot_mode = input_mode::consumeBoot(config, consumed);
	// Consume durably before exposing USB. A failed write must not boot a
	// personality whose one-shot intent would be replayed on every power cycle.
	if (saved_boot.once != input_mode::NO_BOOT_MODE && !saveSettings(config, consumed)) return false;
	running_mode = boot_mode;
	return true;
}

bool saveInputConfig(const input_mode::Config &config)
{
	return saveSettings(config, {running_mode, input_mode::NO_BOOT_MODE});
}
bool saveBootMode(input_mode::Mode mode)
{
	if (uint8_t(mode) > 1) return false;
	return saveSettings(saved_config, input_mode::bootRequest(saved_config, mode));
}
static bool saveSettings(const input_mode::Config &config, input_mode::BootState boot)
{
	if (!input_mode::valid(config)) return false;
	alignas(4) uint8_t bytes[input_mode::RECORD_SIZE];
	input_mode::encodeRecord(config, config_generation + 1, bytes, boot);
	uint32_t next = config_page == CONFIG_A ? CONFIG_B : CONFIG_A;
	if (radio_running) radio::pause();
	bool ok = program(next, bytes, sizeof(bytes));
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
	ok = ready(100000) && ok;
	__DSB(); __ISB();
	if (radio_running) radio::resume();
	input_mode::Config verified{};
	input_mode::BootState verified_boot;
	uint32_t read_generation = 0;
	ok = ok && input_mode::decodeRecord(reinterpret_cast<const uint8_t *>(next), verified, read_generation, &verified_boot)
		&& read_generation == config_generation + 1 && verified_boot.last == boot.last && verified_boot.once == boot.once;
	if (ok) { config_page = next; ++config_generation; saved_config = config; saved_boot = boot; }
	return ok;
}

} // namespace storage
