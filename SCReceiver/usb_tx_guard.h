#pragma once
#include <rtos.h>

namespace usb {
// Loop-task USB submissions must finish nRF5x's EasyDMA claim/start window
// before the higher-priority usbd task can spin on that same DMA owner.
// Keep interrupts enabled: RADIO/TIMER service must retain its IRQ latency.
class TxGuard {
	UBaseType_t saved_;
public:
	TxGuard() : saved_(uxTaskPriorityGet(nullptr))
	{
		if (saved_ < TASK_PRIO_HIGH) vTaskPrioritySet(nullptr, TASK_PRIO_HIGH);
	}
	~TxGuard()
	{
		if (saved_ < TASK_PRIO_HIGH) vTaskPrioritySet(nullptr, saved_);
	}
	TxGuard(const TxGuard &) = delete;
	TxGuard &operator=(const TxGuard &) = delete;
};
}
