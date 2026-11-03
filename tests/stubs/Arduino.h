#pragma once
#include <stdint.h>
inline uint32_t mock_micros;
struct MockPowerRegisters { uint32_t USBREGSTATUS = 1; };
inline MockPowerRegisters mock_power;
#define NRF_POWER (&mock_power)
struct MockFicrRegisters { uint32_t DEVICEID[2] = {0x12345678, 0x9abcdef0}; };
inline MockFicrRegisters mock_ficr;
#define NRF_FICR (&mock_ficr)
#define POWER_USBREGSTATUS_VBUSDETECT_Msk 1u
inline uint32_t micros() { return mock_micros; }
inline uint32_t __get_PRIMASK() { return 0; }
inline void __disable_irq() {}
inline void __enable_irq() {}
inline void __DMB() {}
inline void delay(unsigned) {}
