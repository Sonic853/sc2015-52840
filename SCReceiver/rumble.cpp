#include "rumble.h"
#include "protocol.h"
#include <string.h>

namespace rumble {
void Mailbox::stop()
{
	if (!++epoch_) ++epoch_;
	slot_ = -1; session_ = 0; pending_ = false;
}
uint32_t Mailbox::target(int slot, uint32_t session, bool active)
{
	if (!active || slot < 0 || slot >= 4 || !session) {
		if (slot_ >= 0 || pending_) stop();
		return 0;
	}
	if (slot_ != slot || session_ != session) pending_ = false;
	slot_ = slot; session_ = session;
	return epoch_;
}
void Mailbox::submit(uint8_t low, uint8_t high, uint32_t at)
{
	if (slot_ < 0) return;
	command_ = {slot_, session_, epoch_, at, low, high}; pending_ = true;
}
bool Mailbox::take(Command &command)
{
	if (!pending_) return false;
	command = command_; pending_ = false; return true;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void pulse(bool left, uint8_t strength, uint8_t out[10])
{
	memset(out, 0, 10);
	out[0] = 0x8f; out[1] = 8; out[2] = left ? 1 : 0;
	// Two finite pulse trains approximate the two ERM motors on SC2015 pads.
	// Strength controls pulse width, with a maximum 20% duty cycle, gain 0.
	uint16_t period = left ? 5000 : 2500;
	uint16_t on = uint16_t((uint32_t(strength) * (period / 5) + 254) / 255);
	put16(out + 3, on); put16(out + 5, uint16_t(period - on));
	put16(out + 7, strength ? uint16_t(PULSE_US / period) : 0);
}
void Bridge::select(sc::Receiver &receiver, int slot, uint32_t session, uint32_t usb_generation)
{
	if (slot_ == slot && session_ == session && usb_generation_ == usb_generation) return;
	if (slot_ >= 0) receiver.cancelRumble(unsigned(slot_), session_);
	slot_ = slot; session_ = session; usb_generation_ = usb_generation;
	low_ = high_ = 0; sent_ = false;
}
bool Bridge::accept(sc::Receiver &receiver, const Command &command)
{
	if (slot_ < 0 || command.slot != slot_ || command.session != session_ ||
	    command.usb_generation != usb_generation_) return false;
	if (low_ != command.low || high_ != command.high)
		receiver.cancelRumble(unsigned(slot_), session_);
	low_ = command.low; high_ = command.high; at_ = command.at_us;
	return true;
}
void Bridge::service(sc::Receiver &receiver, uint32_t now)
{
	if (slot_ < 0) return;
	if (!receiver.connected(unsigned(slot_)) || receiver.pending(unsigned(slot_)) ||
	    receiver.sessionGeneration(unsigned(slot_)) != session_ || receiver.importing() ||
	    uint32_t(now - at_) >= HOST_TIMEOUT_US) {
		receiver.cancelRumble(unsigned(slot_), session_); low_ = high_ = 0;
	}
	if (!(low_ || high_)) return;
	if ((!sent_ || uint32_t(now - last_sent_) >= REFRESH_US) &&
	    receiver.queueRumble(unsigned(slot_), session_, low_, high_)) {
		last_sent_ = now; sent_ = true;
	}
}
}
