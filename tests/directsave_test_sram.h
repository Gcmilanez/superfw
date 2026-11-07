#include <stdint.h>

extern uint8_t fake_sram[0x10000];

#define SRAM_BASE ((uintptr_t)fake_sram)
