#pragma once
#include <array>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <device/dcd.h>

#define CFG_TUD_HID 5
#define CFG_TUD_VENDOR 0
#define HID_ITF_PROTOCOL_KEYBOARD 1
enum { HID_PROTOCOL_BOOT = 0, HID_PROTOCOL_REPORT = 1 };
enum hid_report_type_t { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_OUTPUT = 2,
	HID_REPORT_TYPE_FEATURE = 3 };
enum xfer_result_t { XFER_RESULT_SUCCESS, XFER_RESULT_FAILED };
enum { CONTROL_STAGE_SETUP, CONTROL_STAGE_DATA, CONTROL_STAGE_ACK };

struct tusb_desc_device_t {
	uint8_t bLength, bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
	uint16_t idVendor, idProduct, bcdDevice;
	uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

struct tusb_control_request_t {
	uint8_t bmRequestType, bRequest;
	uint16_t wValue, wIndex, wLength;
};
extern "C" void dcd_edpt0_status_complete(uint8_t, const tusb_control_request_t *);
extern "C" void tud_event_hook_cb(uint8_t, uint32_t, bool);
extern "C" bool tud_control_xfer(uint8_t, const tusb_control_request_t *, void *, uint16_t);
inline const uint8_t *mock_control_pointer = nullptr;
inline std::vector<uint8_t> mock_control_input;
bool mockFeatureSet(unsigned interface_number, const uint8_t *data, unsigned requested,
		    unsigned actual, xfer_result_t result = XFER_RESULT_SUCCESS);
bool mockFeatureData(unsigned interface_number, const uint8_t *data, unsigned requested, unsigned actual);
bool mockFeatureAck();

class Adafruit_USBD_Interface {
public:
	uint8_t interface_number = 0;
	virtual ~Adafruit_USBD_Interface() = default;
	virtual uint16_t getInterfaceDescriptor(uint8_t, uint8_t *, uint16_t) = 0;
};
struct MockDevice {
	uint16_t vid = 0, pid = 0, device_version = 0;
	bool mounted_state = true, suspended_state = false;
	bool unplug_on_detach = false;
	unsigned detach_count = 0, interface_count = 0, in_count = 1, out_count = 1;
	std::vector<std::vector<uint8_t>> descriptors;
	void detach() {
		++detach_count;
		// nRF5x TinyUSB emits this even though VBUS is still present.
		if (unplug_on_detach) tud_event_hook_cb(0, DCD_EVENT_UNPLUGGED, false);
	}
	bool attach() { return true; }
	void clearConfiguration() { interface_count = 0; in_count = out_count = 1; descriptors.clear(); }
	uint8_t allocInterface(uint8_t count = 1) { uint8_t n = uint8_t(interface_count); interface_count += count; return n; }
	uint8_t allocEndpoint(uint8_t in) { return uint8_t(in ? (0x80 | in_count++) : out_count++); }
	bool addInterface(Adafruit_USBD_Interface &itf) {
		uint8_t number = uint8_t(interface_count), bytes[256]{};
		uint16_t size = itf.getInterfaceDescriptor(number, bytes, sizeof(bytes));
		if (!size) return false;
		if (interface_count == number) ++interface_count;
		itf.interface_number = number;
		descriptors.emplace_back(bytes, bytes + size);
		return true;
	}
	void setConfigurationBuffer(uint8_t *, unsigned) {}
	void setID(uint16_t v, uint16_t p) { vid = v; pid = p; }
	void setVersion(uint16_t) {}
	void setDeviceVersion(uint16_t value) { device_version = value; }
	void setManufacturerDescriptor(const char *) {}
	void setProductDescriptor(const char *) {}
	void setConfigurationAttribute(uint8_t) {}
	void setConfigurationMaxPower(unsigned) {}
	bool mounted() { return mounted_state; }
	bool suspended() { return suspended_state; }
};
inline MockDevice TinyUSBDevice;

class Adafruit_USBD_HID : public Adafruit_USBD_Interface {
public:
	using Get = uint16_t (*)(uint8_t, hid_report_type_t, uint8_t *, uint16_t);
	using Set = void (*)(uint8_t, hid_report_type_t, const uint8_t *, uint16_t);
	inline static std::vector<Adafruit_USBD_HID *> instances;
	std::vector<std::vector<uint8_t>> sent;
	const uint8_t *report = nullptr;
	unsigned report_size = 0;
	uint8_t protocol = 0, interval = 0;
	// Descriptor boot protocol and host-selected report format are separate.
	uint8_t protocol_mode = HID_PROTOCOL_REPORT;
	bool strict_in_flight = false, in_flight = false;
	Get get_callback = nullptr;
	Set set = nullptr;
	uint16_t get(uint8_t id, hid_report_type_t type, uint8_t *out, uint16_t length,
		     bool complete = true)
	{
		uint16_t count = get_callback(id, type, out, length);
		if (count && complete) {
			tusb_control_request_t request{0xa1, 1, uint16_t((type << 8) | id),
				interface_number, length};
			dcd_edpt0_status_complete(0, &request);
		}
		return count;
	}
	virtual ~Adafruit_USBD_HID() = default;
	virtual uint16_t getInterfaceDescriptor(uint8_t number, uint8_t *buffer, uint16_t capacity)
	{
		if (buffer && capacity >= 25) {
			memset(buffer, 0, 25);
			buffer[0] = 9; buffer[1] = 4; buffer[2] = number;
			buffer[5] = 3; buffer[6] = protocol ? 1 : 0; buffer[7] = protocol;
			buffer[9] = 9; buffer[10] = 0x21;
			buffer[18] = 7; buffer[19] = 5; buffer[20] = uint8_t(0x81 + number);
			buffer[22] = 64; buffer[24] = interval;
		}
		return 25;
	}
	void setReportDescriptor(const uint8_t *data, uint16_t size) { report = data; report_size = size; }
	void setBootProtocol(uint8_t value) { protocol = value; }
	uint8_t getProtocol() { return protocol_mode; }
	void setPollInterval(uint8_t value) { interval = value; }
	void setReportCallback(Get g, Set s) { get_callback = g; set = s; }
	bool begin() { if (!TinyUSBDevice.addInterface(*this)) return false; instances.push_back(this); return true; }
	void completeReport() { in_flight = false; }
	bool ready() { return !strict_in_flight || !in_flight; }
	bool sendReport(uint8_t id, const void *data, uint8_t size)
	{
		if (!ready())
			return false;
		const uint8_t *bytes = static_cast<const uint8_t *>(data);
		std::vector<uint8_t> packet;
		if (id) packet.push_back(id);
		packet.insert(packet.end(), bytes, bytes + size);
		sent.push_back(packet);
		if (strict_in_flight)
			in_flight = true;
		return true;
	}
};
