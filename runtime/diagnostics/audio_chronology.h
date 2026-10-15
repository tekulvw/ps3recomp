/* Default-off, bounded metadata chronology. Never establishes logical PCM ownership. */
#ifndef PS3RECOMP_AUDIO_CHRONOLOGY_H
#define PS3RECOMP_AUDIO_CHRONOLOGY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Port IDs are registry indices, not title-specific addresses. Epoch increments
 * on every open/close. Token state is diagnostic-only, outside spu_context. */
typedef struct audio_chronology_dma_token {
    uint64_t begin_us, copy_us, epoch, serial;
    uint64_t ea;
    uint32_t size, slot_mask, offset, bytes, nonzero, words, overlap;
    int port;
} audio_chronology_dma_token;
#define AUDIO_CHRONOLOGY_CALL_SPAN_VERSION 1
/* Separate metadata-only stream, sharing the audio capture clock/window. */
typedef struct audio_chronology_call_token {
    uint64_t serial, run, ctx, thread, args[9], previous_note_scope[4];
    uint32_t target, depth;
} audio_chronology_call_token;
void audio_chronology_call_begin(audio_chronology_call_token*, const void*, uint32_t, uint32_t);
void audio_chronology_call_end(audio_chronology_call_token*, unsigned);
#define AUDIO_CHRONOLOGY_WAIT_NOTES_VERSION 1
/* Same selected-call/window scope; no register or guest-memory accesses. */
typedef struct audio_chronology_wait_token {
    uint64_t serial, run, span, ctx, thread, requested;
} audio_chronology_wait_token;
void audio_chronology_wait_begin(audio_chronology_wait_token*, uint64_t);
void audio_chronology_wait_end(const audio_chronology_wait_token*, int, int);
void audio_chronology_workload(uint64_t, uint32_t, uint64_t, int32_t, uint32_t, uint32_t, uint32_t);
#define AUDIO_CHRONOLOGY_SCALAR_NOTES_VERSION 1
/* Optional caller-supplied raw scalars in the existing selected-call stream.
 * Scope/token numbers are diagnostic identities, never guest/job ownership. */
uint64_t audio_chronology_scalar_scope(void);
uint64_t audio_chronology_scalar(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
int audio_chronology_enabled(void);
void audio_chronology_port(unsigned, uint32_t, uint32_t, uint32_t, int);
void audio_chronology_consume(unsigned, unsigned, uint64_t, const uint8_t*, uint32_t, uint64_t);
void audio_chronology_dma_begin(audio_chronology_dma_token*, uint64_t, uint32_t, uint32_t);
void audio_chronology_dma_after(audio_chronology_dma_token*, const uint8_t*);
void audio_chronology_dma_end(const audio_chronology_dma_token*, const void*, uint32_t, uint32_t, uint32_t, uint32_t, int, unsigned);
void audio_chronology_backend(uint32_t, uint32_t, int, uint32_t);
void audio_chronology_notify(uint32_t, uint64_t, int);
void audio_chronology_api(unsigned, unsigned, uint64_t, uint64_t, uint64_t, int);
void audio_chronology_poll(void);
void audio_chronology_finish(void);
#ifdef __cplusplus
}
#endif
#endif
