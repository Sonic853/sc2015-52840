#pragma once
#include <stdint.h>

namespace input_mode {

enum class Mode : uint8_t { Steam = 0, Xbox = 1 };
// Wire IDs also used by the configurator. Zero disables a physical button.
enum Target : uint8_t { None, A, B, X, Y, LB, RB, Back, Start, Guide,
	L3, R3, Up, Down, Left, Right, LT, RT, TargetCount };
constexpr unsigned BUTTON_COUNT = 16;
constexpr unsigned CONFIG_SIZE = 24;
constexpr unsigned RECORD_SIZE = 48;
struct Config {
	uint8_t bindings[BUTTON_COUNT];
	uint16_t stick_deadzone, pad_deadzone, dpad_threshold;
	uint8_t flags; // bit0/1 invert right-pad X/Y; bit2 requires left-pad click
};
Config defaults();
bool valid(const Config &config);
void encodeConfig(const Config &config, uint8_t out[CONFIG_SIZE]);
bool decodeConfig(const uint8_t in[CONFIG_SIZE], Config &config);
void encodeRecord(const Config &config, uint32_t generation, uint8_t out[RECORD_SIZE]);
bool decodeRecord(const uint8_t in[RECORD_SIZE], Config &config, uint32_t &generation);

class Router {
public:
	Router();
	void reset();
	void configure(const Config &config);
	const Config &config() const { return config_; }
	Mode mode() const { return mode_; }
	int owner() const { return owner_; }
	bool requested(Mode &mode);
	void changeMode(Mode mode);
	// Type-C removal suppresses any held chord until a fresh released input.
	void unplug();
	// Called only for reconstructed protocol input, never CRC-only IRQ data.
	bool observe(unsigned slot, uint32_t generation, bool connected,
		     const uint8_t report[64], uint32_t now_us, bool allow_chord = true);
	bool suppressDesktop() const;
	void xboxReport(uint32_t now_us, uint8_t out[20]);
private:
	struct Slot {
		uint32_t generation, joined_at, held_at, last_at, stick_at, pad_at;
		uint32_t buttons;
		int16_t lx, ly, lpx, lpy, rx, ry;
		uint8_t lt, rt;
		bool connected, seen, holding, consumed, suppressed;
	};
	Slot slots_[4];
	Config config_;
	Mode mode_, request_;
	bool pending_;
	int owner_;
};
}
