#pragma once
#include <array>
#include <stdint.h>
#include <string.h>
#include <vector>

#define CFG_TUD_HID 5
#define HID_ITF_PROTOCOL_KEYBOARD 1
enum hid_report_type_t { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_OUTPUT = 2,
	HID_REPORT_TYPE_FEATURE = 3 };

struct tusb_desc_device_t {
	uint8_t bLength, bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
	uint16_t idVendor, idProduct, bcdDevice;
	uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

struct MockDevice {
	uint16_t vid = 0, pid = 0;
	bool mounted_state = true, suspended_state = false;
	void detach() {}
	bool attach() { return true; }
	void clearConfiguration() {}
	void setConfigurationBuffer(uint8_t *, unsigned) {}
	void setID(uint16_t v, uint16_t p) { vid = v; pid = p; }
	void setVersion(uint16_t) {}
	void setDeviceVersion(uint16_t) {}
	void setManufacturerDescriptor(const char *) {}
	void setProductDescriptor(const char *) {}
	void setConfigurationAttribute(uint8_t) {}
	void setConfigurationMaxPower(unsigned) {}
	bool mounted() { return mounted_state; }
	bool suspended() { return suspended_state; }
};
inline MockDevice TinyUSBDevice;

class Adafruit_USBD_HID {
public:
	using Get = uint16_t (*)(uint8_t, hid_report_type_t, uint8_t *, uint16_t);
	using Set = void (*)(uint8_t, hid_report_type_t, const uint8_t *, uint16_t);
	inline static std::vector<Adafruit_USBD_HID *> instances;
	std::vector<std::vector<uint8_t>> sent;
	const uint8_t *report = nullptr;
	unsigned report_size = 0;
	uint8_t protocol = 0, interval = 0;
	Get get = nullptr;
	Set set = nullptr;
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
	void setPollInterval(uint8_t value) { interval = value; }
	void setReportCallback(Get g, Set s) { get = g; set = s; }
	bool begin() { instances.push_back(this); return true; }
	bool ready() { return true; }
	bool sendReport(uint8_t id, const void *data, uint8_t size)
	{
		const uint8_t *bytes = static_cast<const uint8_t *>(data);
		std::vector<uint8_t> packet;
		if (id) packet.push_back(id);
		packet.insert(packet.end(), bytes, bytes + size);
		sent.push_back(packet);
		return true;
	}
};
