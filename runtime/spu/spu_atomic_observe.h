/* Opt-in bounded diagnostics; no context/generated ABI fields. */
#ifndef SPU_ATOMIC_OBSERVE_H
#define SPU_ATOMIC_OBSERVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;
extern uint64_t g_spu_atomic_observe_state;
int spu_atomic_observe_begin(struct spu_context*,uint32_t,uint32_t,uint64_t);
void spu_atomic_observe_end(uint32_t);
/* Serialized controller only. ARM reserves O_EXCL output before publication. */
int ps3_spu_atomic_observe_arm(uint64_t,uint64_t,const char*);
int ps3_spu_atomic_observe_stop(uint64_t);
int ps3_spu_atomic_observe_dump(uint64_t);
int ps3_spu_atomic_observe_control_request(const char*);
#ifdef __cplusplus
}
#endif
#endif
