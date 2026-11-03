#include "usb.h"
#include "radio.h"
#include "radio_policy.h"
#include "xinput.h"
#include "ps5.h"
#include "usb_tx_guard.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <device/dcd.h>
#include <string.h>

#if CFG_TUD_HID < 5
#error "Five HID instances are required for the original receiver layout"
#endif
#if CFG_TUD_VENDOR
#error "The receiver uses HID vendor reports, not the TinyUSB WebUSB vendor class"
#endif

namespace usb {

// Recovered descriptors at D:0a2a/0aa3. No report ID belongs on a slot's
// 64-byte vendor reports; keyboard/mouse on interface zero use IDs 1/2.
static const uint8_t desktop_descriptor[] = {
	0x05,0x01,0x09,0x06,0x95,0x01,0xa1,0x01,0x85,0x01,0x05,0x07,
	0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,
	0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,
	0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,
	0x91,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,
	0x19,0x00,0x29,0x65,0x81,0x00,0xc0,0x05,0x01,0x09,0x02,0xa1,
	0x01,0x85,0x02,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,
	0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,
	0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,
	0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0xc0,0xc0,
};
static const uint8_t slot_descriptor[] = {
	0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x15,0x00,0x26,0xff,
	0x00,0x75,0x08,0x95,0x40,0x09,0x01,0x81,0x02,0x95,0x40,
	0x09,0x01,0x91,0x02,0x95,0x40,0x09,0x01,0xb1,0x02,0xc0,
};

class Hid : public Adafruit_USBD_HID {
public:
	bool is_desktop = false;
	uint16_t getInterfaceDescriptor(uint8_t number, uint8_t *buffer,
				       uint16_t capacity) override
	{
		uint16_t count = Adafruit_USBD_HID::getInterfaceDescriptor(
			number, buffer, capacity);
		if (buffer && count >= 25) {
			buffer[11] = 0x11;
			buffer[12] = 0x01;
			if (is_desktop) {
				buffer[22] = 9;
				buffer[23] = 0;
			}
		}
		return count;
	}
};

static Hid hid[5];
static uint8_t configuration[256];
static input_mode::Router input_router;
static bool (*persist_input)(const input_mode::Config &);
static bool hid_initialized[5], switching, mode_requested, mode_save_failed, reset_called;
static bool (*persist_mode)(input_mode::Mode);
static void (*restart_receiver)();
static input_mode::Mode requested_mode;
static uint32_t detached_at;
static volatile bool physical_unplug;
static uint32_t initialized_generation[4];
static input_mode::Mode initialized_mode[4];
static uint8_t mode_replies[4][64];
static uint32_t mode_versions[4];
static uint8_t mode_targets[4];
static bool mode_armed[4];
static uint32_t last_xinput_at;
static rumble::Bridge rumble_bridge;
static void inputModeReport(sc::Receiver &receiver, unsigned slot, const uint8_t *data);
static unsigned slotInterfaceBase() { return input_router.mode() != input_mode::Mode::Steam ? 255u : 1u; }
input_mode::Mode inputMode() { return input_router.mode(); }
void configureBoot(input_mode::Mode mode, bool (*persist)(input_mode::Mode), void (*restart)())
{
	input_router.reset(); input_router.changeMode(mode);
	persist_mode = persist; restart_receiver = restart;
	switching = mode_requested = mode_save_failed = reset_called = physical_unplug = false;
	memset(initialized_generation, 0, sizeof(initialized_generation));
	memset(mode_armed, 0, sizeof(mode_armed));
	rumble_bridge = rumble::Bridge(); last_xinput_at = 0;
}
void configureInput(const input_mode::Config &config, bool (*persist)(const input_mode::Config &))
{
	input_router.configure(config); persist_input = persist;
}

struct Command {
	bool get;
	uint8_t slot, size, data[64];
	uint32_t version;
	uint32_t accepted_at;
	bool waited;
};
static Command commands[32];
static volatile uint8_t command_head, command_tail;
static volatile uint8_t pending_sets[4];
static volatile uint8_t pending_gets[4];
static volatile uint8_t pending_haptics[4];
static uint8_t replies[4][64];
static uint32_t reply_versions[4];
static volatile uint32_t command_drops;
static volatile uint32_t stale_session_sets;
static const sc::Receiver *receiver_source;
// EP0 has one control transfer at a time. Capture the exact published version
// at SETUP, but consume it only after TinyUSB completes the status stage.
static bool feature_read_pending;
static bool feature_write_reserved;
static uint32_t feature_write_generation;
static uint32_t feature_write_at;
static volatile uint8_t reset_requested;
static uint8_t feature_read_slot;
static uint32_t feature_read_version;
struct Query {
	uint8_t data[64], size;
	bool submitted;
	sc::QueryStatus status;
	uint32_t generation;
};
static Query queries[4];
static bool slot_connected[4];
static uint8_t *control_out;
static uint16_t control_out_capacity, control_out_actual;
static bool control_out_ok, control_query, control_ack;
struct HostStatistics {
	uint32_t sets, query_busy, query_ready, gets, unpublished_gets, invalid_sets;
	uint8_t last_set_slot, last_set_command, last_set_size;
	uint8_t last_get_slot, last_get_command, last_get_size;
	uint8_t rejected_command, rejected_length, rejected_meta;
};
static HostStatistics host_stats;

struct UsbQueueMetrics {
	uint32_t admitted, processed, haptic_received, haptic_waits, haptic_timeouts;
	uint32_t haptic_stale, haptic_offline, haptic_reset_cancelled, max_age_us;
};
static UsbQueueMetrics usb_queue_metrics[4];
static uint8_t usb_high_water;
static uint32_t max_process_gap, last_process_at, max_rx_latency;
static bool process_seen;
struct QueueSnapshot {
	sc::QueueMetrics rf;
	UsbQueueMetrics usb[4];
	uint32_t id, at_us, rx_latency, process_gap;
	uint8_t depth, high_water, pending_haptics[4];
	bool valid, clock_available;
};
static QueueSnapshot queue_snapshot;
static uint8_t queue_replies[4][64];
static void queueReport(sc::Receiver &receiver, unsigned slot, const uint8_t *data);

static uint8_t commandDepth()
{
	return uint8_t((command_head + 32u - command_tail) % 32);
}

static void queueHighWater()
{
	uint8_t depth = commandDepth();
	if (depth > usb_high_water) usb_high_water = depth;
}

struct Output {
	uint8_t data[8][64];
	uint8_t head, count;
};
static Output outputs[4];
static uint8_t keyboard_state[8];
static bool keyboard_dirty;
static uint8_t mouse_buttons;
static bool mouse_dirty;
static int mouse_motion[3];
static bool desktop_enabled = true;
static bool desktop_mouse_next;
static uint8_t desktop_protocol = HID_PROTOCOL_REPORT;

class Critical {
	uint32_t previous;
public:
	Critical() : previous(__get_PRIMASK()) { __disable_irq(); }
	~Critical() { if (!previous) __enable_irq(); }
};

enum class FeatureRejection : uint8_t {
	None, Frame, LegacyBootloader, Parameters, Transfer, Length,
};

static FeatureRejection featureRejection(const uint8_t *buffer, unsigned count)
{
	if (count < 2 || count > 64 || buffer[1] > 62 || buffer[1] + 2u > count)
		return FeatureRejection::Frame;
	// 96 is a local PRNG entropy hint, not a bootloader operation.
	if (buffer[0] == 0x90)
		return FeatureRejection::LegacyBootloader;
	if ((buffer[0] == 0xb0 && buffer[1] != 24) ||
	    (buffer[0] == 0xb1 && buffer[1] != 2) ||
	    (buffer[0] == 0xaf && (buffer[1] != 4 || memcmp(buffer + 2, "wipe", 4))))
		return FeatureRejection::Parameters;
	return FeatureRejection::None;
}

static bool rejectFeature(unsigned slot, const uint8_t *buffer, unsigned count,
			  FeatureRejection reason)
{
	Critical critical;
	++host_stats.invalid_sets;
	// FA reserved bytes expose only header metadata, never entropy or keys.
	host_stats.rejected_command = count ? buffer[0] : 0;
	host_stats.rejected_length = count >= 2 ? buffer[1] : 0;
	host_stats.rejected_meta = uint8_t((slot << 6) | uint8_t(reason));
	return false;
}

static bool bestEffort(uint8_t command)
{
	return command == 0x8f || (command >= 0xa2 && command <= 0xa6);
}

static bool sessionBound(uint8_t command)
{
	return !sc::localFeature(command) || command == 0xb2 || command == 0xb3;
}

// Caller holds Critical. One record is reserved by the current EP0 transfer.
static bool commandSpace()
{
	unsigned free = (command_tail + 31u - command_head) % 32;
	return free > unsigned(feature_read_pending || feature_write_reserved);
}

// Caller holds Critical. No protocol work, AES or Flash in the USB task.
static bool queueFeature(unsigned slot, const uint8_t *buffer, unsigned count,
			 uint32_t generation = 0, uint32_t accepted_at = radio::queueMicros())
{
	if ((reset_requested & 2) || switching || !receiver_source)
		return false;
	uint8_t next = (command_head + 1) % 32;
	// A full GET reserves one ring record until its status callback. Other
	// producers may not steal that consumption record while EP0 is in flight.
	if (!commandSpace()) {
		++command_drops;
		return false;
	}
	Command &command = commands[command_head];
	command.get = false;
	command.slot = uint8_t(slot);
	command.size = uint8_t(count);
	command.accepted_at = accepted_at;
	command.waited = false;
	command.version = sessionBound(buffer[0]) ?
		(generation ? generation : receiver_source->sessionGeneration(slot)) : 0;
	memcpy(command.data, buffer, count);
	++pending_sets[slot];
	++usb_queue_metrics[slot].admitted;
	if (buffer[0] == 0x8f) {
		++pending_haptics[slot];
		++usb_queue_metrics[slot].haptic_received;
	}
	memset(replies[slot], 0, 64);
	replies[slot][0] = buffer[0];
	reply_versions[slot] = 0;
	__DMB();
	command_head = next;
	queueHighWater();
	return true;
}

static bool querySet(unsigned slot, const uint8_t *buffer, unsigned count)
{
	Critical critical;
	if ((reset_requested & 2) || switching || !receiver_source)
		return false;
	++host_stats.sets;
	host_stats.last_set_slot = uint8_t(slot);
	host_stats.last_set_command = buffer[0];
	host_stats.last_set_size = uint8_t(count);
	Query &query = queries[slot];
	unsigned size = buffer[1] + 2;
	uint32_t generation = receiver_source->sessionGeneration(slot);
	if (query.generation != generation || query.size != size || memcmp(query.data, buffer, size)) {
		query = {};
		query.generation = generation;
		query.size = uint8_t(size);
		memcpy(query.data, buffer, size);
	}
	if (!pending_sets[slot] && query.submitted &&
	    (query.status == sc::QueryStatus::Ready || query.status == sc::QueryStatus::Offline))
	{
		++host_stats.query_ready;
		return true;
	}
	if (!query.submitted) {
		query.status = sc::QueryStatus::Pending;
		query.submitted = queueFeature(slot, buffer, count);
	}
	++host_stats.query_busy;
	return false; // D:19ae: enqueue accepted, but SET status must still STALL.
}

template<unsigned Slot>
static uint16_t getReport(uint8_t id, hid_report_type_t type,
			  uint8_t *buffer, uint16_t requested)
{
	// D:2e6e -> 2f1d returns one zero byte, not the interrupt-IN snapshot.
	if (!id && type == HID_REPORT_TYPE_INPUT && requested) {
		buffer[0] = 0;
		return 1;
	}
	if (id || type != HID_REPORT_TYPE_FEATURE || !requested)
		return 0;
	Critical critical;
	feature_read_pending = false;
	if ((reset_requested & 2) || switching)
		return 0;
	++host_stats.gets;
	// SET's ACK callback queues work for the loop. An immediate host GET must
	// retry, not interpret an unpublished command/length-zero placeholder.
	if (pending_sets[Slot] || pending_gets[Slot] || !reply_versions[Slot]) {
		++host_stats.unpublished_gets;
		return 0;
	}
	unsigned count = requested < 64 ? requested : 64;
	if (count == 64 && (command_head + 1) % 32 == command_tail) {
		++command_drops;
		return 0;
	}
	memcpy(buffer, replies[Slot], count);
	if (replies[Slot][0] < 0xf0 && replies[Slot][0] != 0xa1 && replies[Slot][0] != 0xb4) {
		host_stats.last_get_slot = Slot;
		host_stats.last_get_command = replies[Slot][0];
		host_stats.last_get_size = count > 1 ? replies[Slot][1] : 0;
	}
	// Descriptor/header probes must not consume a reply or arm a reset.
	if (count < 64)
		return uint16_t(count);
	feature_read_slot = Slot;
	feature_read_version = reply_versions[Slot];
	feature_read_pending = true;
	return uint16_t(count);
}

static void controlComplete(const tusb_control_request_t &request)
{
	Critical critical;
	bool consume = !(reset_requested & 2) && feature_read_pending && request.bmRequestType == 0xa1 &&
		request.bRequest == 1 && request.wValue == 0x0300 &&
		request.wLength >= 64 && request.wIndex == feature_read_slot + slotInterfaceBase();
	feature_read_pending = false;
	if (!consume)
		return;
	uint8_t next = (command_head + 1) % 32;
	if (next == command_tail) {
		++command_drops;
		return;
	}
	Command &command = commands[command_head];
	command.get = true;
	command.slot = feature_read_slot;
	command.version = feature_read_version;
	command.accepted_at = radio::queueMicros();
	command.waited = false;
	++pending_gets[feature_read_slot];
	__DMB();
	command_head = next;
	queueHighWater();
	// The USB task can serve another transfer before the loop runs. Retire the
	// consumed publication now, or an identical SET/GET can replay its old reply.
	if (reply_versions[feature_read_slot] == feature_read_version) {
		reply_versions[feature_read_slot] = 0;
		queries[feature_read_slot].status = sc::QueryStatus::None;
		queries[feature_read_slot].submitted = false;
	}
}

template<unsigned Slot>
static void setReport(uint8_t id, hid_report_type_t type,
		      const uint8_t *buffer, uint16_t count)
{
	if (control_ack && type == HID_REPORT_TYPE_FEATURE)
		count = control_out_actual;
	// D:2eec -> 2f10 handles Output locally as a GPIO indication. XIAO has
	// no corresponding pin; it must not execute Output bytes as Feature RPCs.
	if (id || type != HID_REPORT_TYPE_FEATURE ||
	    featureRejection(buffer, count) != FeatureRejection::None)
		return;
	Critical critical;
	if (control_ack)
		feature_write_reserved = false;
	if ((reset_requested & 2) || switching)
		return;
	++host_stats.sets;
	if (buffer[0] < 0xf0 && buffer[0] != 0xa1 && buffer[0] != 0xb4) {
		host_stats.last_set_slot = Slot;
		host_stats.last_set_command = buffer[0];
		host_stats.last_set_size = uint8_t(count);
	}
	queries[Slot] = {};
	queueFeature(Slot, buffer, count, control_ack ? feature_write_generation : 0,
		     control_ack ? feature_write_at : radio::queueMicros());
}

bool begin()
{
	TinyUSBDevice.detach();
	delay(30);
	TinyUSBDevice.clearConfiguration();
	TinyUSBDevice.setConfigurationBuffer(configuration, sizeof(configuration));
	bool xbox = input_router.mode() == input_mode::Mode::Xbox;
	bool dualsense = input_router.mode() == input_mode::Mode::PS5;
	TinyUSBDevice.setID(dualsense ? 0x054c : xbox ? 0x045e : 0x28de, dualsense ? 0x0ce6 : xbox ? 0x028e : 0x1142);
	TinyUSBDevice.setVersion(0x0200);
	TinyUSBDevice.setDeviceVersion(dualsense ? 0x0110 : xbox ? 0x0114 : 0x0001);
	TinyUSBDevice.setManufacturerDescriptor(xbox || dualsense ? "SC2015" : "Valve Software");
	TinyUSBDevice.setProductDescriptor(dualsense ? "SC2015 DualSense Controller" : xbox ? "SC2015 Xbox Controller" : "Steam Controller");
	xinput::enable(xbox);
	ps5::enable(dualsense, NRF_FICR->DEVICEID[0], NRF_FICR->DEVICEID[1]);
	if (xbox) {
		if (!xinput::addInterface()) return false;
		TinyUSBDevice.setConfigurationAttribute(0xa0);
		TinyUSBDevice.setConfigurationMaxPower(500);
		return TinyUSBDevice.attach();
	}
	if (dualsense) {
		hid[0].is_desktop = false;
		hid[0].setBootProtocol(0); hid[0].enableOutEndpoint(true);
		hid[0].setReportDescriptor(ps5::reportDescriptor(), ps5::reportDescriptorSize());
		hid[0].setReportCallback(ps5::getReport, ps5::setReport);
		hid[0].setPollInterval(1);
		if (!(hid_initialized[0] ? TinyUSBDevice.addInterface(hid[0]) : hid[0].begin())) return false;
		hid_initialized[0] = true;
		TinyUSBDevice.setConfigurationAttribute(0xa0);
		TinyUSBDevice.setConfigurationMaxPower(100);
		return TinyUSBDevice.attach();
	}
	hid[0].is_desktop = true;
	hid[0].enableOutEndpoint(false); hid[0].setReportCallback(nullptr, nullptr);
	hid[0].setReportDescriptor(desktop_descriptor, sizeof(desktop_descriptor));
	hid[0].setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
	hid[0].setPollInterval(2);
	if (!(hid_initialized[0] ? TinyUSBDevice.addInterface(hid[0]) : hid[0].begin()))
		return false;
	hid_initialized[0] = true;
	hid[1].setReportCallback(getReport<0>, setReport<0>);
	hid[2].setReportCallback(getReport<1>, setReport<1>);
	hid[3].setReportCallback(getReport<2>, setReport<2>);
	hid[4].setReportCallback(getReport<3>, setReport<3>);
	for (unsigned i = 1; i < 5; ++i) {
		hid[i].setReportDescriptor(slot_descriptor, sizeof(slot_descriptor));
		hid[i].setPollInterval(6);
		if (!(hid_initialized[i] ? TinyUSBDevice.addInterface(hid[i]) : hid[i].begin()))
			return false;
		hid_initialized[i] = true;
	}
	TinyUSBDevice.setConfigurationAttribute(0xa0);
	TinyUSBDevice.setConfigurationMaxPower(100);
	return TinyUSBDevice.attach();
}

bool serviceMode(sc::Receiver &receiver)
{
	uint32_t now = radio::queueMicros();
	bool unplug;
	{
		Critical critical;
		unplug = physical_unplug; physical_unplug = false;
	}
	if (unplug && !switching && !(input_router.config().flags & input_mode::PERSIST_MODE)) {
		mode_requested = input_router.mode() != input_mode::Mode::Steam;
		requested_mode = input_mode::Mode::Steam;
	}
	input_mode::Mode chord_mode;
	if (input_router.requested(chord_mode) && !unplug && !switching && !receiver.importing()) {
		mode_requested = true; requested_mode = chord_mode;
	}
	if (mode_requested && !switching) {
		if (requested_mode == input_router.mode() && !unplug) mode_requested = false;
		else {
			// Only a consumed full FD reply (or a fresh startup chord) can
			// reach persistence. Short/aborted GETs leave no durable intent.
			if (!persist_mode || !restart_receiver || !persist_mode(requested_mode)) {
				mode_save_failed = true; mode_requested = false;
			} else {
				mode_save_failed = false; switching = true;
				TinyUSBDevice.detach(); xinput::enable(false); ps5::enable(false, 0, 0);
				detached_at = radio::queueMicros();
				Critical critical;
				reset_requested |= 3;
				memset(mode_armed, 0, sizeof(mode_armed));
			}
		}
	}
	now = radio::queueMicros();
	if (switching && uint32_t(now - detached_at) >= 60000) {
		if (!reset_called) { reset_called = true; restart_receiver(); }
		return false; // NVIC_SystemReset must not return on hardware.
	}
	int owner = !switching && !mode_requested && !(reset_requested & 2) &&
		!receiver.importing() ? input_router.rumbleOwner(now) : -1;
	if (owner >= 0 && (!receiver.connected(unsigned(owner)) || receiver.pending(unsigned(owner)))) owner = -1;
	uint32_t session = owner >= 0 ? receiver.sessionGeneration(unsigned(owner)) : 0;
	bool dualsense = input_router.mode() == input_mode::Mode::PS5;
	uint32_t usb_generation = dualsense ? ps5::setRumbleTarget(owner, session) : xinput::setRumbleTarget(owner, session);
	rumble_bridge.select(receiver, usb_generation ? owner : -1, session, usb_generation);
	rumble::Command rumble_command;
	if (dualsense ? ps5::takeRumble(rumble_command) : xinput::takeRumble(rumble_command)) rumble_bridge.accept(receiver, rumble_command);
	// An OUT callback can preempt this function after its first clock sample.
	// Sample again after consuming it so a newer receipt cannot look expired.
	rumble_bridge.service(receiver, radio::queueMicros());
	if (switching) return true;
	for (unsigned slot = 0; slot < 4; ++slot) {
		uint32_t generation = receiver.sessionGeneration(slot);
		if (!receiver.connected(slot) || receiver.pending(slot) ||
		    (initialized_generation[slot] == generation && initialized_mode[slot] == input_router.mode()) ||
		    receiver.freeFeatureFragments(slot) < 2) continue;
		// Restore default handset lizard state on every new Steam connection:
		// the receiver may have lost USB power while the handset stayed on.
		bool translated = input_router.mode() != input_mode::Mode::Steam;
		if (receiver.queueInputProfile(slot, generation, translated)) {
			initialized_generation[slot] = generation; initialized_mode[slot] = input_router.mode();
		}
	}
	return true;
}

void observeRxLatency(uint32_t age_us)
{
	if (age_us > max_rx_latency) max_rx_latency = age_us;
}

unsigned process(sc::Receiver &receiver, unsigned budget, uint32_t haptic_wait_us)
{
	uint32_t now = radio::queueMicros();
	if (process_seen && uint32_t(now - last_process_at) > max_process_gap)
		max_process_gap = now - last_process_at;
	last_process_at = now;
	process_seen = true;
	uint8_t reset;
	{
		Critical critical;
		receiver_source = &receiver;
		reset = reset_requested;
		reset_requested = 0;
		if (reset & 2) {
			memset(mode_armed, 0, sizeof(mode_armed));
			for (unsigned i = 0; i < 4; ++i) {
				usb_queue_metrics[i].haptic_reset_cancelled += pending_haptics[i];
				pending_haptics[i] = 0;
			}
			queue_snapshot.valid = false;
			command_head = command_tail = 0;
			memset((void *)pending_sets, 0, sizeof(pending_sets));
			memset((void *)pending_gets, 0, sizeof(pending_gets));
			memset(queries, 0, sizeof(queries));
			memset(reply_versions, 0, sizeof(reply_versions));
			feature_read_pending = feature_write_reserved = false;
			feature_write_generation = 0;
			feature_write_at = 0;
			control_out = nullptr;
			control_out_ok = control_query = false;
		}
	}
	if (reset) {
		for (auto &output : outputs) output.head = output.count = 0;
		desktopEnabled(false);
		desktopEnabled(receiver.desktopEnabled());
		receiver.resetInputs();
	}
	// USB callbacks only copy bounded records. Flash writes, crypto, and
	// protocol state transitions belong to the loop task, never the USB task.
	unsigned processed = 0;
	for (unsigned i = 0; i < budget; ++i) {
		Command command;
		{
			Critical critical;
			if ((reset_requested & 2) || command_tail == command_head)
				break;
			command = commands[command_tail];
		}
		// A successful USB write owns this ring record until the RF FIFO has
		// room for the ENTIRE command. Do not silently lose ordinary settings.
		// Original D:19ce haptics/A2-A6 are best effort: drop/count on RF full.
		bool stale = !command.get && command.version &&
			command.version != receiver.sessionGeneration(command.slot);
		uint32_t age = radio::queueMicros() - command.accepted_at;
		if (!command.get && age > usb_queue_metrics[command.slot].max_age_us)
			usb_queue_metrics[command.slot].max_age_us = age;
		bool full = !stale && !command.get && !sc::localFeature(command.data[0]) &&
			!receiver.canQueueFeature(command.slot, command.data, command.size);
		bool haptic = !command.get && command.data[0] == 0x8f;
		if (full && (!bestEffort(command.data[0]) || haptic)) {
			uint8_t depth;
			{
				Critical critical;
				depth = commandDepth();
				commands[command_tail].waited = true;
			}
			if (!command.waited) {
				if (haptic) ++usb_queue_metrics[command.slot].haptic_waits;
				receiver.queueEvent(command.slot, sc::QueueEventKind::UsbWait, command.data[0],
					uint8_t(command.data[1] + 2), uint8_t((command.data[1] + 28u) / 27), age, depth);
			}
			if (!haptic || age < haptic_wait_us)
				break;
			++usb_queue_metrics[command.slot].haptic_timeouts;
			receiver.queueEvent(command.slot, sc::QueueEventKind::UsbTimeout, command.data[0],
				uint8_t(command.data[1] + 2), uint8_t((command.data[1] + 28u) / 27), age, depth);
			// Capacity is still unavailable. Dispatch once to count/classify the
			// actual RF rejection, then retire this unstarted USB command.
		}
		if (stale) {
			++stale_session_sets;
			if (haptic) ++usb_queue_metrics[command.slot].haptic_stale;
			receiver.queueEvent(command.slot, sc::QueueEventKind::StaleUsb, command.data[0],
				uint8_t(command.data[1] + 2), uint8_t((command.data[1] + 28u) / 27), age);
		} else if (command.get) {
			bool consumed = receiver.consumeFeature(command.slot, command.version);
			if (consumed && mode_armed[command.slot] && mode_versions[command.slot] == command.version) {
				mode_armed[command.slot] = false;
				mode_requested = true; requested_mode = input_mode::Mode(mode_targets[command.slot]);
			}
		} else {
			mode_armed[command.slot] = false;
			if (haptic && !receiver.connected(command.slot))
				++usb_queue_metrics[command.slot].haptic_offline;
			if (command.version)
				receiver.featureForSession(command.slot, command.version, command.data, command.size);
			else
				receiver.feature(command.slot, command.data, command.size);
			if (command.data[0] == 0xb1)
				desktopEnabled(receiver.desktopEnabled());
			if (command.data[0] == 0xfc)
				queueReport(receiver, command.slot, command.data);
			if (command.data[0] == 0xfd || command.data[0] == 0xfe)
				inputModeReport(receiver, command.slot, command.data);
		}
		Critical critical;
		if (command.get) --pending_gets[command.slot];
		else {
			--pending_sets[command.slot];
			++usb_queue_metrics[command.slot].processed;
			if (haptic) --pending_haptics[command.slot];
		}
		command_tail = (command_tail + 1) % 32;
		++processed;
	}
	return processed;
}

static void put32(uint8_t *out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out[i] = uint8_t(value >> (8 * i));
}

static uint32_t get32(const uint8_t *in)
{
	return uint32_t(in[0]) | (uint32_t(in[1]) << 8) |
	       (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}

static void inputModeReport(sc::Receiver &receiver, unsigned slot, const uint8_t *data)
{
	uint8_t *out = mode_replies[slot];
	receiver.peekFeature(slot, out, mode_versions[slot]);
	memset(out, 0, 64);
	out[0] = data[0]; out[1] = 62; out[2] = 1; out[3] = radio::FIRMWARE_REVISION;
	bool busy = switching || mode_requested || receiver.importing();
	if (data[0] == 0xfd) {
		uint8_t target = uint8_t(input_router.mode());
		if (data[1]) {
			if (data[1] != 5 || memcmp(data + 2, "MODE", 4) || !input_mode::validMode(data[6])) out[4] = 1;
			else if (busy) out[4] = 4;
			else {
				target = data[6]; mode_targets[slot] = target;
				mode_armed[slot] = target != uint8_t(input_router.mode());
			}
		}
		out[5] = uint8_t(input_router.mode()); out[6] = target;
		out[7] = input_router.owner() < 0 ? 255 : uint8_t(input_router.owner());
		bool remember = input_router.config().flags & input_mode::PERSIST_MODE;
		out[8] = uint8_t((persist_input ? 1 : 0) | (busy ? 2 : 0) |
			(remember ? 8 : 4) | (mode_save_failed ? 16 : 0));
		xinput::diagnostics(out);
	} else {
		uint8_t wire[input_mode::CONFIG_SIZE];
		input_mode::encodeConfig(input_router.config(), wire);
		uint32_t token = sc::crc32(wire, sizeof(wire));
		if (data[1]) {
			input_mode::Config next{};
			if (data[1] != 29 || data[2] != 1 || !input_mode::decodeConfig(data + 7, next)) out[4] = 1;
			else if (get32(data + 3) != token) out[4] = 2;
			else if (busy) out[4] = 4;
			else if (memcmp(wire, data + 7, sizeof(wire))) {
				if (!persist_input || !persist_input(next)) out[4] = 6;
				else {
					input_router.configure(next); memcpy(wire, data + 7, sizeof(wire));
					token = sc::crc32(wire, sizeof(wire));
				}
			}
		}
		out[5] = input_mode::CONFIG_SIZE; put32(out + 8, token);
		memcpy(out + 12, wire, sizeof(wire));
	}
}

static void queueReport(sc::Receiver &receiver, unsigned slot, const uint8_t *data)
{
	uint8_t page = data[1] ? data[2] : 0;
	uint8_t status = 0;
	if (page > 17 || (page == 0 ? data[1] > 1 : data[1] != 5))
		status = 1;
	else if (page == 0) {
		// Freeze the USB-owned fields and timestamp briefly. Protocol metrics
		// only change in this loop task, so copy them with interrupts enabled,
		// sampling pending ages at the same cutoff rather than a later time.
		{
			Critical critical;
			if (!++queue_snapshot.id) ++queue_snapshot.id;
			queue_snapshot.at_us = radio::queueMicros();
			memcpy(queue_snapshot.usb, usb_queue_metrics, sizeof(usb_queue_metrics));
			for (unsigned i = 0; i < 4; ++i) queue_snapshot.pending_haptics[i] = pending_haptics[i];
			queue_snapshot.depth = commandDepth();
			queue_snapshot.high_water = usb_high_water;
		}
		receiver.copyQueueMetrics(queue_snapshot.rf, queue_snapshot.at_us);
		queue_snapshot.rx_latency = max_rx_latency;
		queue_snapshot.process_gap = max_process_gap;
		queue_snapshot.clock_available = receiver.queueClockAvailable();
		queue_snapshot.valid = true;
	} else if (!queue_snapshot.valid || get32(data + 3) != queue_snapshot.id) {
		status = 2;
	}
	uint8_t *out = queue_replies[slot];
	memset(out, 0, 64);
	out[0] = 0xfc; out[1] = 62; out[2] = 1; out[3] = page;
	out[4] = radio::FIRMWARE_REVISION; out[5] = status;
	put32(out + 6, queue_snapshot.id);
	put32(out + 10, queue_snapshot.at_us);
	if (status) return;
	if (page == 0) {
		out[14] = 12; out[15] = 31;
		out[16] = queue_snapshot.rf.event_count;
		out[17] = queue_snapshot.high_water;
		uint32_t totals[10] = {};
		for (unsigned i = 0; i < 4; ++i) {
			const auto &rf = queue_snapshot.rf.slots[i];
			const auto &usb = queue_snapshot.usb[i];
			totals[0] += rf.haptic_admitted;
			totals[1] += rf.haptic_full;
			for (unsigned j = 0; j < 5; ++j) totals[2] += rf.a2_a6_full[j];
			totals[3] += rf.initial_retries + rf.count_retries;
			totals[4] += usb.haptic_waits;
			totals[5] += usb.haptic_timeouts;
			if (rf.max_fifo_age_us > totals[6]) totals[6] = rf.max_fifo_age_us;
			if (usb.max_age_us > totals[7]) totals[7] = usb.max_age_us;
			if (rf.fifo_high_water > out[62]) out[62] = rf.fifo_high_water;
		}
		totals[8] = queue_snapshot.rx_latency;
		totals[9] = queue_snapshot.process_gap;
		for (unsigned i = 0; i < 10; ++i) put32(out + 18 + 4 * i, totals[i]);
		put32(out + 58, sc::queue_policy::HAPTIC_WAIT_US);
		out[63] = queue_snapshot.clock_available ? 1 : 0;
	} else if (page <= 8) {
		unsigned index = (page - 1) % 4;
		const auto &rf = queue_snapshot.rf.slots[index];
		const auto &usb = queue_snapshot.usb[index];
		if (page <= 4) {
			const uint32_t values[12] = {rf.admitted, rf.haptic_admitted, rf.haptic_full,
				rf.a2_a6_full[0], rf.a2_a6_full[1], rf.a2_a6_full[2], rf.a2_a6_full[3], rf.a2_a6_full[4],
				rf.other_full, rf.initial_retries, rf.count_retries, rf.commit_waits};
			for (unsigned i = 0; i < 12; ++i) put32(out + 14 + 4 * i, values[i]);
		} else {
			const uint32_t values[12] = {rf.offline, rf.serialized, rf.retired, rf.cleared_fragments,
				rf.max_fifo_age_us, rf.max_lane_age_us, usb.admitted, usb.processed,
				usb.haptic_received, usb.haptic_waits, usb.haptic_timeouts, usb.haptic_stale};
			for (unsigned i = 0; i < 12; ++i) put32(out + 14 + 4 * i, values[i]);
		}
		out[62] = rf.fifo_high_water;
		out[63] = queue_snapshot.pending_haptics[index];
	} else if (page <= 16) {
		unsigned first = (page - 9) * 2;
		unsigned oldest = (queue_snapshot.rf.event_head + 16 - queue_snapshot.rf.event_count) % 16;
		for (unsigned i = 0; i < 2 && first + i < queue_snapshot.rf.event_count; ++i) {
			const auto &event = queue_snapshot.rf.events[(oldest + first + i) % 16];
			uint8_t *row = out + 14 + 24 * i;
			put32(row, event.id); put32(row + 4, event.at_us);
			put32(row + 8, event.generation); put32(row + 12, event.age_us);
			const uint8_t metadata[8] = {event.slot, event.kind, event.command, event.length,
				event.needed, event.fifo_count, event.lane_flags, event.usb_depth};
			memcpy(row + 16, metadata, sizeof(metadata));
			++out[62];
		}
	} else {
		for (unsigned i = 0; i < 4; ++i) {
			const auto &usb = queue_snapshot.usb[i];
			put32(out + 14 + 12 * i, usb.max_age_us);
			put32(out + 18 + 12 * i, usb.haptic_offline);
			put32(out + 22 + 12 * i, usb.haptic_reset_cancelled);
		}
		out[62] = queue_snapshot.depth;
		out[63] = sc::queue_policy::USB_BATCH;
	}
}

static void hostDiagnostics(uint8_t report[64])
{
	Critical critical;
	memset(report, 0, 64);
	report[0] = 0xfa; report[1] = 62; report[2] = 1;
	report[3] = report[63] = radio::FIRMWARE_REVISION;
	report[4] = host_stats.last_set_slot;
	report[5] = host_stats.last_set_command;
	report[6] = host_stats.last_set_size;
	report[7] = host_stats.last_get_slot;
	report[8] = host_stats.last_get_command;
	report[9] = host_stats.last_get_size;
	report[10] = host_stats.rejected_command;
	report[11] = host_stats.rejected_length;
	report[62] = host_stats.rejected_meta;
	const uint32_t values[] = {host_stats.sets, host_stats.query_busy, host_stats.query_ready,
		host_stats.gets, host_stats.unpublished_gets, host_stats.invalid_sets, command_drops};
	for (unsigned i = 0; i < 7; ++i) put32(report + 12 + 4 * i, values[i]);
	for (unsigned i = 0; i < 4; ++i) {
		unsigned offset = 40 + 5 * i;
		report[offset] = queries[i].size ? queries[i].data[0] : 0;
		report[offset + 1] = queries[i].size;
		report[offset + 2] = uint8_t(queries[i].status);
		report[offset + 3] = replies[i][0];
		report[offset + 4] = replies[i][1];
		if (slot_connected[i]) report[60] |= 1u << i;
		if (pending_sets[i]) report[61] |= 1u << i;
	}
}

void publish(const sc::Receiver &receiver)
{
	{
		Critical critical;
		receiver_source = &receiver;
	}
	for (unsigned slot = 0; slot < 4; ++slot) {
		uint8_t snapshot[64];
		uint32_t version;
		Query query;
		{
			Critical critical;
			query = queries[slot];
		}
		sc::QueryStatus status = receiver.queryStatus(slot, query.data, query.size);
		receiver.peekFeature(slot, snapshot, version);
		if (query.size && status == sc::QueryStatus::Offline && snapshot[0] != query.data[0]) {
			memset(snapshot, 0, sizeof(snapshot));
			snapshot[0] = query.data[0];
		}
		if (snapshot[0] == 0xf0) {
			const radio::Statistics &s = radio::statistics();
			snapshot[1] = 62;
			put32(snapshot + 35, s.crc_ok);
			put32(snapshot + 39, s.crc_bad);
			put32(snapshot + 43, s.queue_full);
			put32(snapshot + 47, s.late_ack);
			put32(snapshot + 51, s.faults);
			put32(snapshot + 55, command_drops);
		}
		if (snapshot[0] == 0xf7)
			radio::pairingDiagnostics(snapshot);
		if (snapshot[0] == 0xfa)
			hostDiagnostics(snapshot);
		if (snapshot[0] == 0xfb) {
			put32(snapshot + 24, radio::statistics().ack_deferred);
			put32(snapshot + 28, stale_session_sets);
		}
		if (snapshot[0] == 0xfc)
			memcpy(snapshot, queue_replies[slot], 64);
		if (snapshot[0] == 0xfd || snapshot[0] == 0xfe)
			memcpy(snapshot, mode_replies[slot], 64);
		Critical critical;
		slot_connected[slot] = receiver.connected(slot);
		// A GET may complete after process() but before this publication pass.
		// Do not resurrect its old Receiver cache until consumption has run.
		if (!(reset_requested & 2) && !pending_sets[slot] && !pending_gets[slot]) {
			memcpy(replies[slot], snapshot, 64);
			reply_versions[slot] = version;
			queries[slot].status = status;
			if (status == sc::QueryStatus::None)
				queries[slot].submitted = false;
		}
	}
}

bool observeInput(unsigned slot, const uint8_t report[64])
{
	return receiver_source && input_router.observe(slot, receiver_source->sessionGeneration(slot),
		receiver_source->connected(slot), report, radio::queueMicros(), !receiver_source->pending(slot));
}

void input(unsigned slot, const uint8_t report[64])
{
	if (slot >= 4)
		return;
	if (receiver_source && report[2] == 3)
		input_router.observe(slot, receiver_source->sessionGeneration(slot), receiver_source->connected(slot), report, radio::queueMicros());
	if (input_router.mode() != input_mode::Mode::Steam) return;
	Output &output = outputs[slot];
	if (output.count == 8) {
		// Coalesce superseded input, but retain connection notifications when
		// possible. Transport acknowledgement does not wait for USB readiness.
		unsigned last = (output.head + output.count - 1) % 8;
		if (report[2] == 1 && output.data[last][2] == 1) {
			memcpy(output.data[last], report, 64);
			return;
		}
		output.head = (output.head + 1) % 8;
		--output.count;
	}
	memcpy(output.data[(output.head + output.count) % 8], report, 64);
	++output.count;
}

void desktop(const uint8_t keyboard[8], const uint8_t mouse[4])
{
	const uint8_t neutral[8] = {};
	if (input_router.suppressDesktop()) { keyboard = neutral; mouse = neutral; }
	if (memcmp(keyboard, keyboard_state, 8)) {
		memcpy(keyboard_state, keyboard, 8);
		if (desktop_enabled)
			keyboard_dirty = true;
	}
	if (desktop_enabled && mouse_buttons != mouse[0])
		mouse_dirty = true;
	mouse_buttons = mouse[0];
	// Keep the latest absolute snapshot: B1's protocol hook can run before
	// process() synchronizes the enable flag. Relative motion while disabled
	// or in Boot mode must never be replayed when Report mode resumes.
	if (!desktop_enabled || hid[0].getProtocol() == HID_PROTOCOL_BOOT) {
		memset(mouse_motion, 0, sizeof(mouse_motion));
		return;
	}
	for (unsigned i = 0; i < 3; ++i) {
		mouse_motion[i] += int8_t(mouse[i + 1]);
		if (mouse_motion[i] > 32767)
			mouse_motion[i] = 32767;
		if (mouse_motion[i] < -32767)
			mouse_motion[i] = -32767;
		mouse_dirty |= mouse_motion[i] != 0;
	}
}

void desktopEnabled(bool enable)
{
	if (!enable) {
		memset(keyboard_state, 0, sizeof(keyboard_state));
		mouse_buttons = 0;
		memset(mouse_motion, 0, sizeof(mouse_motion));
		desktop_mouse_next = false;
	}
	if (enable != desktop_enabled) {
		// On disable, send at most one neutral report per desktop report type.
		// Repeated synchronization must not re-arm an already sent release.
		// On enable, publish the latest snapshot even without a new input packet.
		keyboard_dirty = mouse_dirty = true;
	}
	desktop_enabled = enable;
}

void pump()
{
	if (switching) return;
	if (!TinyUSBDevice.mounted()) {
		for (auto &output : outputs)
			output.head = output.count = 0;
		return;
	}
	if (input_router.mode() != input_mode::Mode::Steam) {
		if (TinyUSBDevice.suspended()) return;
		TxGuard guard;
		bool dualsense = input_router.mode() == input_mode::Mode::PS5;
		if (!dualsense) xinput::service();
		uint32_t now = radio::queueMicros();
		if (uint32_t(now - last_xinput_at) >= 4000) {
			uint8_t report[20]; input_router.xboxReport(now, report);
			if (dualsense) {
				if (hid[0].ready()) {
					uint8_t payload[63]; ps5::buildReport(report, now, payload);
					if (hid[0].sendReport(1, payload, sizeof(payload))) last_xinput_at = now;
				}
			} else if (xinput::send(report)) last_xinput_at = now;
		}
		return;
	}
	uint8_t protocol = hid[0].getProtocol();
	bool boot = protocol == HID_PROTOCOL_BOOT;
	if (protocol != desktop_protocol) {
		desktop_protocol = protocol;
		keyboard_dirty = true;
		mouse_dirty = !boot;
		desktop_mouse_next = false;
	}
	if (boot) {
		// Standard interface-zero Boot keyboard, not the original bit1 quirk.
		memset(mouse_motion, 0, sizeof(mouse_motion));
		mouse_dirty = false;
	}
	if (TinyUSBDevice.suspended())
		return;
	TxGuard guard;
	xinput::service();
	for (unsigned i = 0; i < 4; ++i) {
		Output &output = outputs[i];
		if (output.count && hid[i + 1].ready() &&
		    hid[i + 1].sendReport(0, output.data[output.head], 64)) {
			output.head = (output.head + 1) % 8;
			--output.count;
		}
	}
	if (!hid[0].ready())
		return;
	// One shared IN endpoint: alternate successful submissions when both
	// reports are pending, even if the keyboard changes before every poll.
	if (!boot && mouse_dirty && (!keyboard_dirty || desktop_mouse_next)) {
		uint8_t report[4] = {uint8_t(desktop_enabled ? mouse_buttons : 0)};
		for (unsigned i = 0; i < 3; ++i)
			if (desktop_enabled)
				report[i + 1] = uint8_t(mouse_motion[i] < -127 ? -127 :
						      mouse_motion[i] > 127 ? 127 : mouse_motion[i]);
		if (hid[0].sendReport(2, report, 4)) {
			mouse_dirty = false;
			for (unsigned i = 0; i < 3; ++i) {
				mouse_motion[i] -= int8_t(report[i + 1]);
				mouse_dirty |= mouse_motion[i] != 0;
			}
			desktop_mouse_next = false;
		}
	} else if (keyboard_dirty) {
		const uint8_t neutral[8] = {};
		if (hid[0].sendReport(boot ? 0 : 1,
				      desktop_enabled ? keyboard_state : neutral, 8)) {
			keyboard_dirty = false;
			desktop_mouse_next = true;
		}
	}
}

} // namespace usb

static bool slotFeatureSet(const tusb_control_request_t &request)
{
	return request.bmRequestType == 0x21 && request.bRequest == 9 &&
	       request.wValue == 0x0300 && request.wIndex >= usb::slotInterfaceBase() &&
	       request.wIndex < usb::slotInterfaceBase() + 4;
}
static bool ps5Set(const tusb_control_request_t &request)
{
	return usb::inputMode() == input_mode::Mode::PS5 && request.bmRequestType == 0x21 && request.bRequest == 9;
}

extern "C" bool __real_tud_control_xfer(uint8_t, const tusb_control_request_t *, void *, uint16_t);
extern "C" bool __wrap_tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request,
				       void *buffer, uint16_t count)
{
	bool ok = __real_tud_control_xfer(rhport, request, buffer, count);
	if (ok && (slotFeatureSet(*request) || ps5Set(*request))) {
		usb::control_out = static_cast<uint8_t *>(buffer);
		usb::control_out_capacity = count;
	}
	return ok;
}

extern "C" bool __real_usbd_control_xfer_cb(uint8_t, uint8_t, xfer_result_t, uint32_t);
extern "C" bool __wrap_usbd_control_xfer_cb(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t count)
{
	// Slot SETs fit in one EP0 packet. TinyUSB passes wLength to its HID
	// callback, even after a short OUT; track actual bytes to reject stale tails.
	if (ep == 0 && usb::control_out) {
		usb::control_out_ok = result == XFER_RESULT_SUCCESS && count <= usb::control_out_capacity;
		usb::control_out_actual = uint16_t(count);
	}
	return __real_usbd_control_xfer_cb(rhport, ep, result, count);
}

extern "C" bool __real_hidd_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
extern "C" bool __wrap_hidd_control_xfer_cb(uint8_t rhport, uint8_t stage,
					 const tusb_control_request_t *request)
{
	if (stage == CONTROL_STAGE_SETUP) {
		usb::Critical critical;
		usb::control_out = nullptr;
		usb::control_out_actual = 0;
		usb::control_out_ok = usb::control_query = false;
		usb::feature_read_pending = false;
		usb::feature_write_reserved = false;
		usb::feature_write_generation = 0;
		usb::feature_write_at = 0;
		// Zero wLength bypasses TinyUSB's DATA validation and goes straight to
		// ACK. Reject it before the HID callback can see a previous buffer tail.
		if (slotFeatureSet(*request) && (request->wLength < 2 || request->wLength > 64))
			return usb::rejectFeature(request->wIndex - usb::slotInterfaceBase(), nullptr, 0, usb::FeatureRejection::Length);
	}
	if (ps5Set(*request)) {
		if (request->wIndex != 0 || request->wValue != 0x0202 || request->wLength < 48 || request->wLength > 64 ||
		    (usb::reset_requested & 2) || usb::switching) return false;
		if (stage == CONTROL_STAGE_DATA || stage == CONTROL_STAGE_ACK) {
			if (!usb::control_out || !usb::control_out_ok || usb::control_out_actual != request->wLength || usb::control_out[0] != 2)
				return false;
		}
		return __real_hidd_control_xfer_cb(rhport, stage, request);
	}
	if (slotFeatureSet(*request)) {
		{
			usb::Critical critical;
			// ISR reset notification precedes loop cleanup. Do not acknowledge
			// new work that would be erased when that cleanup finally runs.
			if ((usb::reset_requested & 2) || usb::switching || !usb::receiver_source)
				return false;
		}
		if (stage == CONTROL_STAGE_DATA) {
			if (!usb::control_out || !usb::control_out_ok)
				return usb::rejectFeature(request->wIndex - usb::slotInterfaceBase(), nullptr, 0, usb::FeatureRejection::Transfer);
			usb::FeatureRejection reason = usb::featureRejection(usb::control_out, usb::control_out_actual);
			if (reason != usb::FeatureRejection::None)
				return usb::rejectFeature(request->wIndex - usb::slotInterfaceBase(), usb::control_out, usb::control_out_actual, reason);
			usb::control_query = sc::featureQuery(usb::control_out[0]);
			if (usb::control_query)
				return usb::querySet(request->wIndex - usb::slotInterfaceBase(), usb::control_out, usb::control_out_actual);
			// Reserve before acknowledging SET, not in its void ACK callback.
			// A full ring now produces a retryable STALL with no side effects.
			usb::Critical critical;
			if (!usb::commandSpace()) {
				++usb::command_drops;
				return false;
			}
			usb::feature_write_reserved = true;
			usb::feature_write_generation = usb::receiver_source->sessionGeneration(request->wIndex - usb::slotInterfaceBase());
			usb::feature_write_at = radio::queueMicros();
		}
		if (stage == CONTROL_STAGE_ACK) {
			if (!usb::control_out_ok)
				return false;
			// Query DATA already queued the first request. A successful SET retry
			// acknowledges readiness without clearing or requeueing its reply.
			if (usb::control_query)
				return true;
			if (!usb::feature_write_reserved)
				return false;
			usb::control_ack = true;
			bool ok = __real_hidd_control_xfer_cb(rhport, stage, request);
			usb::control_ack = false;
			return ok;
		}
	}
	return __real_hidd_control_xfer_cb(rhport, stage, request);
}

// TinyUSB's nRF5 DCD has no platform implementation of this weak hook.
// It runs at TinyUSB's status completion stage, unlike GET_REPORT at SETUP.
// nRF5x synthesizes status completion when queued, not from a hardware ACK IRQ.
extern "C" void dcd_edpt0_status_complete(uint8_t, const tusb_control_request_t *request)
{
	usb::controlComplete(*request);
}

// The event hook may run in the ISR. Protocol/input reset is deferred to loop.
extern "C" void tud_event_hook_cb(uint8_t, uint32_t event, bool)
{
	usb::Critical critical;
	if (event == DCD_EVENT_BUS_RESET || event == DCD_EVENT_UNPLUGGED)
		usb::reset_requested |= 3;
	else if (event == DCD_EVENT_RESUME)
		usb::reset_requested |= 1;
	if (event == DCD_EVENT_BUS_RESET || event == DCD_EVENT_UNPLUGGED || event == DCD_EVENT_SUSPEND) {
		xinput::stopRumble(); ps5::stopRumble();
	}
	if (event == DCD_EVENT_BUS_RESET || event == DCD_EVENT_UNPLUGGED) ps5::reset();
	// nRF5x dcd_disconnect() also emits UNPLUGGED for a software detach.
	// Only absent VBUS means Type-C was removed. Latch it here so a quick
	// replug before the loop runs still restores the Steam personality.
	if (event == DCD_EVENT_UNPLUGGED &&
	    !(NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk))
		usb::physical_unplug = true;
}

// Original USB2.0 receiver exposes no BOS/WebUSB/MS OS capabilities. Use
// linker wrappers: TinyUSB's public declarations otherwise inherit weak linkage.
extern "C" const uint8_t *__wrap_tud_descriptor_bos_cb(void) { return nullptr; }
extern "C" bool __wrap_tud_vendor_control_xfer_cb(uint8_t port, uint8_t stage, const tusb_control_request_t *request)
{
	return xinput::vendorControl(port, stage, request);
}

extern "C" const uint16_t *__real_tud_descriptor_string_cb(uint8_t, uint16_t);
extern "C" const uint16_t *__wrap_tud_descriptor_string_cb(uint8_t index, uint16_t language)
{
	if (index == 0xee) return xinput::osString();
	if (index == 4 && usb::inputMode() == input_mode::Mode::Xbox) return xinput::securityString();
	return __real_tud_descriptor_string_cb(index, language);
}

extern "C" const uint8_t *__real_tud_descriptor_device_cb(void);
extern "C" const uint8_t *__wrap_tud_descriptor_device_cb(void)
{
	static tusb_desc_device_t descriptor;
	memcpy(&descriptor, __real_tud_descriptor_device_cb(), sizeof(descriptor));
	descriptor.bDeviceClass = descriptor.bDeviceSubClass = descriptor.bDeviceProtocol =
		usb::inputMode() == input_mode::Mode::Xbox ? 0xff : 0;
	descriptor.iSerialNumber = 0;
	return reinterpret_cast<const uint8_t *>(&descriptor);
}
