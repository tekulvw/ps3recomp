#ifndef PS3RECOMP_GCM_CHECKED_COMPLETION_H
#define PS3RECOMP_GCM_CHECKED_COMPLETION_H
#ifdef __cplusplus
extern "C" {
#endif
/* Optional synchronous completion for a renderer outside the shared engine.
 * Both callbacks return exactly 1 on success. flush submits all earlier GPU
 * work and verifies completion; timeout, device loss, partial primitives and
 * submission errors return 0. Configure under the consumer lease before the
 * first drain; unregister with three NULLs before destroying the renderer.
 * Pending references prevent replacement. Registration never clears a latched
 * checked-stream failure. With no provider the shared engine is unchanged. */
int cellGcm_set_checked_completion(void* user, int (*ready)(void*),
                                  int (*flush)(void*));
#ifdef __cplusplus
}
#endif
#endif
