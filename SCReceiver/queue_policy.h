#pragma once
#include <stdint.h>

namespace sc {
namespace queue_policy {
constexpr unsigned USB_BATCH = 4;
constexpr unsigned RX_BATCH = 8;
constexpr unsigned SERVICE_ROUNDS = 4;
// Extra RF-capacity wait is bounded from USB DATA admission. This is not
// an end-to-end haptic deadline: queued/serialized RF fragments are retained.
constexpr uint32_t HAPTIC_WAIT_US = 8000;
}
}
