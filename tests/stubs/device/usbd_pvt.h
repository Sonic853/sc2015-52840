#pragma once
#include <Adafruit_TinyUSB.h>
#include <rtos.h>
#pragma pack(push, 1)
struct tusb_desc_interface_t {
	uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
	uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
};
struct tusb_desc_endpoint_t {
	uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval;
};
#pragma pack(pop)
struct usbd_class_driver_t {
	const char *name;
	void (*init)(); bool (*deinit)(); void (*reset)(uint8_t);
	uint16_t (*open)(uint8_t, const tusb_desc_interface_t *, uint16_t);
	bool (*control_xfer_cb)(uint8_t, uint8_t, const tusb_control_request_t *);
	bool (*xfer_cb)(uint8_t, uint8_t, xfer_result_t, uint32_t);
	void (*sof)(uint8_t, uint32_t);
};
namespace mock_xinput {
inline bool opened[256]{}, busy[256]{}, claimed[256]{};
inline std::vector<std::vector<uint8_t>> sent;
inline bool fail_open = false, fail_transfer = false;
inline uint8_t *out_buffer;
inline uint16_t out_capacity;
inline unsigned out_attempts;
inline UBaseType_t last_in_priority, last_out_priority;
inline void reset() {
	memset(opened, 0, sizeof(opened)); memset(busy, 0, sizeof(busy));
	memset(claimed, 0, sizeof(claimed)); sent.clear(); fail_open = fail_transfer = false;
	out_buffer = nullptr; out_capacity = 0; out_attempts = 0;
}
}
inline bool usbd_edpt_open(uint8_t, const tusb_desc_endpoint_t *ep) {
	if (mock_xinput::fail_open) return false;
	mock_xinput::opened[ep->bEndpointAddress] = true; return true;
}
inline void usbd_edpt_close(uint8_t, uint8_t ep) { mock_xinput::opened[ep] = false; }
inline bool usbd_edpt_claim(uint8_t, uint8_t ep) {
	if (!mock_xinput::opened[ep] || mock_xinput::busy[ep] || mock_xinput::claimed[ep]) return false;
	mock_xinput::claimed[ep] = true; return true;
}
inline bool usbd_edpt_release(uint8_t, uint8_t ep) { mock_xinput::claimed[ep] = false; return true; }
inline bool usbd_edpt_xfer(uint8_t, uint8_t ep, uint8_t *bytes, uint16_t size) {
	if (ep & 0x80) mock_xinput::last_in_priority = mock_task_priority;
	else { ++mock_xinput::out_attempts; mock_xinput::last_out_priority = mock_task_priority; }
	if (!mock_xinput::opened[ep] || mock_xinput::fail_transfer) return false;
	mock_xinput::busy[ep] = true; mock_xinput::claimed[ep] = false;
	if (ep & 0x80) mock_xinput::sent.emplace_back(bytes, bytes + size);
	else { mock_xinput::out_buffer = bytes; mock_xinput::out_capacity = size; }
	return true;
}
