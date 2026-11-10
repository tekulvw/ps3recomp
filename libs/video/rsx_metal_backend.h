/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * Mirrors the entry-point shape of rsx_d3d12_backend.h so a host can select a
 * backend at build time without any other change. Unlike the D3D12 backend this
 * file is NOT gated on a platform macro: the implementation lives in
 * rsx_metal_backend.m and is only compiled into the library on Apple targets
 * (see CMakeLists.txt), so there is nothing to stub out elsewhere.
 *
 * Headless mode: set PS3RECOMP_METAL_HEADLESS=1 to render to an offscreen
 * MTLTexture with no window or NSApplication. Everything below behaves
 * identically, which is what makes the backend testable in CI on a machine
 * with no display.
 *
 * Host-surface mode: with PS3RECOMP_HOST_SURFACES set (the launcher in
 * tools/host_window sets it) the window belongs to the launcher process and
 * this process opens none and never touches NSApplication. Frames are
 * rendered on the GPU into IOSurfaces the launcher shares, and the launcher
 * shows them. Because the launcher never execs, its window stays up across
 * an exitspawn: the new image finds the launcher through the same variable.
 * See rsx_host_surface.h. If the launcher cannot be reached the backend
 * falls back to a window of its own.
 */
#ifndef PS3RECOMP_RSX_METAL_BACKEND_H
#define PS3RECOMP_RSX_METAL_BACKEND_H

#include "ps3emu/ps3types.h"
#include "rsx_frontend.h"
#include "rsx_native_owner.h"
#include "rsx_native_image.h"
#include "rsx_native_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the device, surface and command queue, and register the backend with
 * the RSX command processor. Returns 0 on success, -1 on failure. */
int  rsx_metal_backend_init(u32 width, u32 height, const char* title);

/* Lazily borrow the existing device/queue for explicit semantic compute.
 * NULL before backend initialization; no change to existing rendering paths. */
rsx_frontend_device *rsx_metal_backend_frontend(void);

/* Unregister, then tear down the surface and device. Safe if init failed. */
void rsx_metal_backend_shutdown(void);

/* Before replacing the process image (exitspawn): put the window's frame and
 * fullscreen state in PS3RECOMP_WINDOW_STATE so the next executable's window
 * opens in the same place. No-op headless, in host-surface mode (the window
 * outlives the exec there) or without a window. */
void rsx_metal_backend_export_window_state(void);

/* Pump the native event queue. Returns -1 when the window has been closed,
 * 0 otherwise. Always returns 0 in headless mode. In host-surface mode the
 * window is the launcher's: -1 once it is closing or the launcher is gone. */
int  rsx_metal_backend_pump_messages(void);

/* Present one frame: clear the drawable to the colour last received through the
 * RSX clear method, then hand it to the compositor. */
void rsx_metal_backend_present(void);

/* Optional diagnostics: PS3RECOMP_METAL_FPS=1 adds display FPS and frame times
 * to the native window title and stderr once per second. Call this exactly once
 * per guest FLIP submission, excluding boot redraws. It never drives rendering.
 * Without calls the separate guest submission rate stays zero. Headless mode
 * does not claim any display presentations and leaves the tracker disabled.
 * PS3RECOMP_METAL_FPS_DIAGNOSTICS=1 additionally reports GPU frame completions
 * and raw callback/drop signals; GPU completion never counts as display FPS. */
void rsx_metal_backend_note_guest_frame(void);
/* A coherent guest FLIP receipt tags one existing present call. ID zero is
 * unknown. PS3RECOMP_METAL_FPS_WINDOW_CONTROL enables opt-in frame ledgers;
 * write a strictly increasing request ID and duration (seconds) to that file.
 * Results use the exact host wall interval and a two-second callback grace. */
void rsx_metal_backend_present_guest_frame(uint64_t guest_flip_id, uint32_t buffer_id);

/* --- test hooks ---------------------------------------------------------- */

/* The clear colour most recently set via NV4097_SET_COLOR_CLEAR_VALUE, in the
 * RSX's native ARGB8888. Lets a host assert the guest command stream actually
 * reached the backend. */
u32  rsx_metal_backend_debug_color(void);

/* Read the centre pixel of the last presented frame as BGRA8888, or 0 if
 * nothing has been presented. Headless mode only -- returns 0 with a live
 * drawable, whose contents belong to the compositor once presented. */
u32  rsx_metal_backend_readback_center(void);

/* How many draws of the last presented frame ran the guest's own vertex and
 * fragment programs rather than the built-in shader. A host mode that loads
 * programs asserts this is non-zero, because the built-in path would draw
 * the same test triangle and a pixel check alone could not tell them apart. */
u32  rsx_metal_backend_guest_draws(void);

/* Commit already-recorded original work before a source-qualified native/UI
 * handoff. No GPU wait: refuses active primitives, tracing and a full async
 * receipt queue.0accepted,1indeterminate (never replay), negative uncommitted.
 * Caller owns the serialized GCM consumer and is outside the owner queue gate.
 * Does not present, select/create a target, or enable original async defaults. */
int rsx_metal_backend_native_boundary(uint64_t *original_ticket);

/* Borrow existing native ownership only; no device/queue is created. */
int rsx_metal_backend_borrow_owner(rsx_native_owner *);
int rsx_metal_backend_borrow_image(uint64_t owner_lifetime,uint32_t backend_surface,rsx_native_image *);
/* Existing HostImage identity/lifetime; call only under its native owner lease.
 * Checks actual queued full clears after the ordered original boundary. */
int rsx_metal_backend_validate_image_planes(void*,uint64_t,uint64_t,uint64_t,uint32_t);
/* Opt-in host diagnostic only. Caller holds a genuine native owner lease and
 * a retained exact HostImage. Same-queue blit waits at most five seconds; no
 * original recording/cache epochs are changed. Native byte order is reported.
 * Zero status proves this diagnostic copy completed, never presentation. */
typedef struct rsx_metal_native_readback {
    uint32_t abi_version,struct_size,width,height;
    uint64_t pixel_format;
    uint32_t storage_mode,row_bytes,padded_row_bytes,reserved;
    uint64_t bytes_written;
    uint32_t gpu_status,gpu_completed;
} rsx_metal_native_readback;
#define RSX_METAL_NATIVE_READBACK_MAX_BYTES UINT64_C(8388608)
static inline int rsx_metal_native_readback_layout(uint32_t width,uint32_t height,
    uint32_t *row_bytes,uint32_t *padded_row_bytes,uint64_t *bytes) {
    if(!width||!height||!row_bytes||!padded_row_bytes||!bytes||
       width>RSX_METAL_NATIVE_READBACK_MAX_BYTES/4u)return -1;
    const uint64_t row=(uint64_t)width*4u,padded=(row+255u)&~UINT64_C(255);
    if(padded>RSX_METAL_NATIVE_READBACK_MAX_BYTES/height)return -1;
    *row_bytes=(uint32_t)row;*padded_row_bytes=(uint32_t)padded;*bytes=row*height;
    return 0;
}
int rsx_metal_backend_read_native_image(void*,uint64_t,uint64_t,uint64_t,
    uint32_t,uint32_t,void*,uint64_t,rsx_metal_native_readback*);
int rsx_metal_backend_reserve_frame(const rsx_native_image *,const rsx_native_frame_source *,rsx_native_frame *);
int rsx_metal_backend_cancel_frame(uint64_t owner_lifetime,uint64_t reservation);
int rsx_metal_backend_poll_frames(rsx_native_frame_event *,uint32_t capacity,uint32_t *count);

int rsx_metal_backend_owner_shutdown_requested(uint64_t lifetime);
int rsx_metal_backend_owner_retry_shutdown(uint64_t lifetime);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_METAL_BACKEND_H */
