/* Host-only OSK bridge. No guest addresses cross onto AppKit's thread. */
#ifndef PS3RECOMP_OSK_NATIVE_H
#define PS3RECOMP_OSK_NATIVE_H
#include <stdint.h>
#if (defined(__APPLE__) && !defined(PS3RECOMP_DISABLE_NATIVE_OSK)) || defined(PS3RECOMP_TEST_NATIVE_OSK)
#define PS3RECOMP_NATIVE_OSK 1
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
 uint64_t token; uint32_t limit, panels, prohibit;
 uint16_t message[513], initial[513];
} OskNativeRequest;
int osk_native_available(void);
void osk_native_macos_set_window(void* window);
void osk_native_open(const OskNativeRequest* request);
void osk_native_close(uint64_t token);
/* UI thread publishes only host-owned data. Events are emitted by the guest pump. */
int osk_native_is_pending(uint64_t token);
void osk_native_did_open(uint64_t token);
void osk_native_host_closed(uint64_t token);
void osk_native_did_finish(uint64_t token,int32_t result,const uint16_t* text,uint32_t length);
/* True only for the current generation with a native OK/Cancel result. */
int osk_native_has_ui_result(uint64_t token);
int32_t osk_native_load(uint32_t param,uint32_t input);
int32_t osk_native_unload(uint32_t result);
int32_t osk_native_get_text(uint32_t result);
int32_t osk_native_abort(void);
void cellOskDialog_tick(void);
#ifdef __cplusplus
}
#endif
#endif
