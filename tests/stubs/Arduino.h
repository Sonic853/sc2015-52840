#pragma once
#include <stdint.h>
inline uint32_t mock_micros;
inline uint32_t micros() { return mock_micros; }
inline uint32_t __get_PRIMASK() { return 0; }
inline void __disable_irq() {}
inline void __enable_irq() {}
inline void __DMB() {}
inline void delay(unsigned) {}
