#pragma once
#include <stdint.h>
#include "motion.h"
#include "lizard.h"

namespace input_mode {

enum class Mode : uint8_t { Steam = 0, Xbox = 1, PS5 = 2, Switch = 3, Lizard = 4 };
constexpr bool validMode(uint8_t value) { return value <= uint8_t(Mode::Lizard); }
// Wire IDs also used by the configurator. Zero disables a physical button.
enum Target : uint8_t { None, A, B, X, Y, LB, RB, Back, Start, Guide,
	L3, R3, Up, Down, Left, Right, LT, RT, TargetCount };
constexpr unsigned BUTTON_COUNT = 16;
constexpr unsigned CONFIG_SIZE = 24;
constexpr unsigned RECORD_SIZE = 48;
constexpr uint8_t PERSIST_MODE = 8, NO_BOOT_MODE = 0xff;
struct BootState {
	Mode last;
	uint8_t once;
};
BootState defaultBootState();
struct Config {
	uint8_t bindings[BUTTON_COUNT];
	uint16_t stick_deadzone, pad_deadzone, dpad_threshold;
	uint8_t flags; // low nibble: inversion/click/persist; high nibble: diagonal angle
};
Config defaults();
uint8_t diagonalDegrees(const Config &config);
bool setDiagonalDegrees(Config &config, uint8_t degrees);
bool valid(const Config &config);
void encodeConfig(const Config &config, uint8_t out[CONFIG_SIZE]);
bool decodeConfig(const uint8_t in[CONFIG_SIZE], Config &config);
BootState bootRequest(const Config &config, Mode mode);
Mode consumeBoot(const Config &config, BootState &boot);
void encodeRecord(const Config &config, uint32_t generation, uint8_t out[RECORD_SIZE],
		  BootState boot = {Mode::Steam, NO_BOOT_MODE});
bool decodeRecord(const uint8_t in[RECORD_SIZE], Config &config, uint32_t &generation, BootState *boot = nullptr);

class Router {
public:
	Router();
	void reset();
	void configure(const Config &config);
	const Config &config() const { return config_; }
	Mode mode() const { return mode_; }
	int owner() const { return owner_; }
	int rumbleOwner(uint32_t now_us) const;
	motion::Sample motionSample(uint32_t now_us) const;
	lizard::Report lizardReport(uint32_t now_us);
	void lizardMouseSent(int x, int y, int wheel);
	bool requested(Mode &mode);
	void changeMode(Mode mode);
	void clearMotion();
	// Type-C removal suppresses any held chord until a fresh released input.
	void unplug();
	// Called only for reconstructed protocol input, never CRC-only IRQ data.
	bool observe(unsigned slot, uint32_t generation, bool connected,
		     const uint8_t report[64], uint32_t now_us, bool allow_chord = true, uint8_t motion_updated = 0);
	bool suppressDesktop() const;
	void xboxReport(uint32_t now_us, uint8_t out[20]);
private:
	struct Slot {
		uint32_t generation, joined_at, held_at, last_at, stick_at, pad_at;
		uint32_t buttons, chord_mask;
		int16_t lx, ly, lpx, lpy, rx, ry;
		uint8_t lt, rt;
		bool connected, seen, holding, consumed, suppressed;
		motion::Tracker motion;
	};
	Slot slots_[4];
	Config config_;
	Mode mode_, request_;
	bool pending_;
	int owner_;
	lizard::Mapper lizard_;
};
}
