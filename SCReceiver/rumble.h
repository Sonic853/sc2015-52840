#pragma once
#include <stdint.h>
namespace sc { class Receiver; }

namespace rumble {
constexpr uint32_t REFRESH_US = 50000, HOST_TIMEOUT_US = 2500000;
constexpr uint32_t PULSE_US = 60000, QUEUE_MAX_AGE_US = 100000;
struct Command {
	int slot;
	uint32_t session, usb_generation, at_us;
	uint8_t low, high;
};
// SC2015 8F pulse encoding, not the SC2/Deck EB rumble command.
void pulse(bool left, uint8_t strength, uint8_t out[10]);

// USB producer/loop consumer. Callers hold their short IRQ critical section.
class Mailbox {
public:
	void stop();
	uint32_t target(int slot, uint32_t session, bool active);
	void submit(uint8_t low, uint8_t high, uint32_t at_us);
	bool take(Command &command);
private:
	int slot_ = -1;
	uint32_t session_ = 0, epoch_ = 1;
	bool pending_ = false;
	Command command_{};
};

class Bridge {
public:
	void select(sc::Receiver &, int slot, uint32_t session, uint32_t usb_generation);
	bool accept(sc::Receiver &, const Command &);
	void service(sc::Receiver &, uint32_t now_us);
private:
	int slot_ = -1;
	uint32_t session_ = 0, usb_generation_ = 0, at_ = 0, last_sent_ = 0;
	uint8_t low_ = 0, high_ = 0;
	bool sent_ = false;
};
}
