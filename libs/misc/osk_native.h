/* Host-only OSK bridge. No guest addresses cross onto AppKit's thread. */
#ifndef PS3RECOMP_OSK_NATIVE_H
#define PS3RECOMP_OSK_NATIVE_H
#include <stdint.h>
#if ((defined(__APPLE__) || (defined(_WIN32) && defined(PS3RECOMP_NATIVE_OSK_WINDOWS)) || \
      (!defined(_WIN32) && !defined(__APPLE__) && \
       (defined(PS3RECOMP_NATIVE_OSK_SDL) || defined(PS3RECOMP_NATIVE_OSK_HEADLESS)))) && \
     !defined(PS3RECOMP_DISABLE_NATIVE_OSK)) || defined(PS3RECOMP_TEST_NATIVE_OSK)
#define PS3RECOMP_NATIVE_OSK 1
#endif
/* Linux/other POSIX: the SDL2 dialog host (osk_sdl.c). It owns the four host
 * entry points and hands a request to the headless host (osk_headless.c) only
 * when no SDL window is registered and that host is built too. */
#if !defined(_WIN32) && !defined(__APPLE__) && defined(PS3RECOMP_NATIVE_OSK_SDL) && \
    !defined(PS3RECOMP_DISABLE_NATIVE_OSK) && !defined(PS3RECOMP_TEST_NATIVE_OSK)
#define OSK_NATIVE_HAS_SDL_HOST 1
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
/* Optional host test control always routes through the real native OK button. */
void osk_native_confirm(uint64_t token);
int osk_native_is_open(uint64_t token);
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
#if defined(_WIN32) && defined(PS3RECOMP_NATIVE_OSK_WINDOWS) && !defined(PS3RECOMP_DISABLE_NATIVE_OSK)
#define OSK_NATIVE_HAS_WIN32_HOST 1
void osk_native_win32_set_window(void* window);
void osk_native_win32_pump(void);
int osk_native_win32_handle_message(void* message);
int osk_native_win32_input_active(void);
#endif
#ifdef OSK_NATIVE_HAS_SDL_HOST
/* Called only on the thread that owns SDL video and pumps its events (the
 * window owner). set_window(NULL) retires any dialog before the window goes. */
void osk_native_sdl_set_window(void* sdl_window);
void osk_native_sdl_pump(void);
/* Returns nonzero when the event belonged to the dialog (the caller drops it). */
int osk_native_sdl_handle_event(const void* sdl_event);
/* Any thread: a dialog is queued or open and owns the keyboard. */
int osk_native_sdl_input_active(void);
#ifdef PS3RECOMP_NATIVE_OSK_HEADLESS
int osk_headless_available(void);
void osk_headless_open(const OskNativeRequest* request);
void osk_headless_close(uint64_t token);
void osk_headless_confirm(uint64_t token);
#endif
#endif
#ifdef __cplusplus
}
#endif
#endif
