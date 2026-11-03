#include <Adafruit_TinyUSB.h>
#include "protocol.h"
#include "radio.h"
#include "storage.h"
#include "usb.h"

static sc::Receiver receiver;
static bool healthy;

static void fatal()
{
	healthy = false;
	for (;;) {
		digitalWrite(LED_RED, LOW);
		delay(150);
		digitalWrite(LED_RED, HIGH);
		delay(150);
	}
}

void setup()
{
	pinMode(LED_RED, OUTPUT);
	pinMode(LED_GREEN, OUTPUT);
	pinMode(LED_BLUE, OUTPUT);
	digitalWrite(LED_RED, HIGH);
	digitalWrite(LED_GREEN, HIGH);
	digitalWrite(LED_BLUE, HIGH);
	pinMode(D0, INPUT_PULLUP);
	if (!usb::begin())
		fatal();
	sc::Database database{};
	if (!storage::load(database))
		fatal();
	sc::Hooks hooks = {radio::aes, radio::random, storage::save,
			   usb::input, usb::desktop, radio::queueMicros};
	if (!receiver.begin(database, hooks))
		fatal();
	usb::publish(receiver);
	bool has_bond = false;
	for (const auto &bond : database.bonds)
		has_bond |= bond.used;
	receiver.pairing(!has_bond || digitalRead(D0) == LOW, true);
	receiver.service();
	radio::publish(receiver);
	if (!radio::begin(database.address))
		fatal();
	storage::radioRunning(true);
	NRF_WDT->CONFIG = WDT_CONFIG_SLEEP_Run;
	NRF_WDT->CRV = 8 * 32768;
	NRF_WDT->RREN = 1;
	NRF_WDT->TASKS_START = 1;
	healthy = true;
}

void loop()
{
	if (!healthy)
		return;
	NRF_WDT->RR[0] = 0x6e524635;
	for (unsigned round = 0; round < sc::queue_policy::SERVICE_ROUNDS; ++round) {
		radio::Received item;
		for (unsigned i = 0; i < sc::queue_policy::RX_BATCH && radio::pop(item); ++i) {
			usb::observeRxLatency(radio::now() - item.timestamp);
			receiver.advanceToHop(item.hop);
			receiver.receive(item.pipe, item.packet);
		}
		uint32_t idle_hop;
		if (radio::idleHops(idle_hop)) receiver.advanceToHop(idle_hop);
		receiver.service();
		radio::publish(receiver);
		usb::process(receiver);
		if (receiver.takeBootloaderRequest()) {
			// TinyUSB signalled completion; nRF's DCD reports status when queued.
			// Give the host time to finish/release handles, outside the USB task.
			delay(100);
			radio::pause();
			TinyUSBDevice.detach();
			enterUf2Dfu();
		}
		if (receiver.takeIdentityChange()) {
			radio::pause();
			radio::publish(receiver);
			receiver.resetHopClock();
			if (!radio::begin(receiver.database().address))
				fatal();
		}
		receiver.service();
		radio::publish(receiver);
	}
	static uint32_t pressed_at;
	static bool button_armed = true;
	if (digitalRead(D0) == LOW) {
		if (!pressed_at)
			pressed_at = millis();
		if (button_armed && uint32_t(millis() - pressed_at) >= 500) {
			receiver.pairing(true, true);
			button_armed = false;
		}
	} else {
		pressed_at = 0;
		button_armed = true;
	}
	receiver.service();
	radio::publish(receiver);
	usb::publish(receiver);
	usb::pump();
	bool any_connected = false;
	for (unsigned i = 0; i < 4; ++i)
		any_connected |= receiver.connected(i);
	digitalWrite(LED_BLUE, receiver.pairing() && (millis() % 500 < 250) ? LOW : HIGH);
	digitalWrite(LED_GREEN, any_connected ? LOW : HIGH);
}
