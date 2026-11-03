#pragma once

#include <stdint.h>

namespace sc {

// Fixed-size, loop-task-owned diagnostics. Counters survive session/transport
// resets and wrap naturally; begin() starts a fresh measurement interval.
struct QueueSlotMetrics {
	// Logical Feature commands, excluding local/offline dispatch and query reuse.
	uint32_t admitted, haptic_admitted, haptic_full;
	uint32_t a2_a6_full[5];
	// Internal retries and commit waits count failed capacity checks.
	uint32_t other_full, initial_retries, count_retries, commit_waits;
	// The last three counters count fragments; bootstrap ACK cache is separate.
	uint32_t offline, serialized, retired, cleared_fragments;
	uint32_t max_fifo_age_us, max_lane_age_us;
	uint8_t fifo_high_water;
};

enum class QueueEventKind : uint8_t {
	HapticFull = 1, BestEffortFull = 2, OtherFull = 3,
	InitialRetry = 4, CountRetry = 5, UsbWait = 6,
	UsbTimeout = 7, StaleUsb = 8, Offline = 9,
};

// Metadata only: length is the logical frame length, needed is its fragment
// count. lane_flags uses bits 0/1 for valid lanes and 2/3 for phase witnesses.
struct QueueEvent {
	uint32_t id, at_us, generation, age_us;
	uint8_t slot, kind, command, length, needed, fifo_count, lane_flags, usb_depth;
};
static_assert(sizeof(QueueEvent) == 24, "queue event metadata must be 24 bytes");

struct QueueMetrics {
	QueueSlotMetrics slots[4]{};
	QueueEvent events[16]{};
	uint32_t next_event_id = 1;
	// Next write position; oldest is (event_head + 16 - event_count) % 16.
	uint8_t event_head = 0, event_count = 0;
};

} // namespace sc
