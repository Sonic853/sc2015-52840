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
			   usb::input, usb::desktop};
	if (!receiver.begin(database, hooks))
		fatal();
	bool has_bond = false;
	for (const auto &bond : database.bonds)
		has_bond |= bond.used;
	receiver.pairing(!has_bond || digitalRead(D0) == LOW);
	receiver.service();
	for (unsigned pipe = 0; pipe < 6; ++pipe)
		for (unsigned parity = 0; parity < 2; ++parity)
			radio::setAck(pipe, parity, receiver.acknowledgement(pipe, parity));
	radio::setPipes(receiver.activePipes());
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
	usb::process(receiver);
	if (receiver.takeIdentityChange()) {
		radio::pause();
		for (unsigned pipe = 0; pipe < 6; ++pipe)
			for (unsigned parity = 0; parity < 2; ++parity)
				radio::setAck(pipe, parity, receiver.acknowledgement(pipe, parity));
		radio::setPipes(receiver.activePipes());
		if (!radio::begin(receiver.database().address))
			fatal();
	}
	radio::Received item;
	for (unsigned i = 0; i < 16 && radio::pop(item); ++i)
		receiver.receive(item.pipe, item.packet);
	uint32_t hops = radio::takeHops();
	for (uint32_t i = 0; i < hops; ++i)
		receiver.hopTick();
	static uint32_t pressed_at;
	static bool button_armed = true;
	if (digitalRead(D0) == LOW) {
		if (!pressed_at)
			pressed_at = millis();
		if (button_armed && uint32_t(millis() - pressed_at) >= 500) {
			receiver.pairing(true);
			button_armed = false;
		}
	} else {
		pressed_at = 0;
		button_armed = true;
	}
	receiver.service();
	for (unsigned pipe = 0; pipe < 6; ++pipe)
		for (unsigned parity = 0; parity < 2; ++parity)
			radio::setAck(pipe, parity, receiver.acknowledgement(pipe, parity));
	radio::setPipes(receiver.activePipes());
	usb::publish(receiver);
	usb::pump();
	bool any_connected = false;
	for (unsigned i = 0; i < 4; ++i)
		any_connected |= receiver.connected(i);
	digitalWrite(LED_BLUE, receiver.pairing() && (millis() % 500 < 250) ? LOW : HIGH);
	digitalWrite(LED_GREEN, any_connected ? LOW : HIGH);
}
