/* Opt-in recorder of the complete observable interface of one SPU job run:
 * the local store and registers at entry, every MFC transfer with its data,
 * every atomic line operation, and the local store and registers at exit.
 * It observes only. It never alters a transfer, a timing decision or guest
 * state, and a context that is not armed pays one relaxed pointer compare. */
#ifndef PS3_JOB_IO_RECORDER_H
#define PS3_JOB_IO_RECORDER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;
extern void *ps3_jobio_active;
static inline int ps3_jobio_is(const void *ctx){return ctx&&__atomic_load_n(&ps3_jobio_active,__ATOMIC_RELAXED)==ctx;}
enum {PS3_JOBIO_GET=1,PS3_JOBIO_PUT=2,PS3_JOBIO_GETLLAR=3,PS3_JOBIO_PUTLLC=4,PS3_JOBIO_PUTLLUC=5,PS3_JOBIO_ZERO=6};
/* Arms one context. Returns 0 on success, nonzero if another run is armed or
 * the directory is unusable. `tag` is an opaque caller value stored in the file. */
int ps3_jobio_begin(struct spu_context *ctx,uint32_t tag,const char *directory);
/* `bytes` is the transferred data (for GET, as loaded into local store; for
 * PUT, as sent). `extra` carries the atomic status for PUTLLC. */
void ps3_jobio_event(struct spu_context *ctx,uint32_t kind,uint32_t cmd,uint32_t lsa,uint64_t ea,uint32_t size,const uint8_t *bytes,uint32_t extra);
/* Finishes the run and writes the file; returns 0 on success, -1 otherwise. */
int ps3_jobio_end(struct spu_context *ctx);
#ifdef __cplusplus
}
#endif
#endif
