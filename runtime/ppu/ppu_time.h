#ifndef PS3RECOMP_PPU_TIME_H
#define PS3RECOMP_PPU_TIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Same clock/domain/units as sys_time_get_system_time; never rebased per caller. */
uint64_t ppu_system_time_us(void);
#ifdef __cplusplus
}
#endif
#endif
