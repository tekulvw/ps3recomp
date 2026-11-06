/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU commands to Direct3D 12 rendering.
 * Implements the rsx_backend callback interface.
 *
 * Architecture:
 *   rsx_commands.c (state tracking) → rsx_backend callbacks → this file
 *                                                               ↓
 *                                                         D3D12 device
 *                                                         swap chain
 *                                                         command lists
 *                                                         pipeline states
 *
 * Current scope (Phase 1):
 *   - Device/swap chain creation
 *   - Clear to RSX clear color
 *   - Present with vsync
 *   - Basic triangle rendering (vertex position + color)
 *
 * Future phases:
 *   - Texture upload and sampling
 *   - RSX vertex/fragment program → HLSL translation
 *   - Framebuffer resolve and display buffer management
 *   - Depth/stencil buffer creation
 */

#ifndef PS3RECOMP_RSX_D3D12_BACKEND_H
#define PS3RECOMP_RSX_D3D12_BACKEND_H
#include <stdint.h>

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the D3D12 backend.
 * Creates a Win32 window, D3D12 device, swap chain, and registers
 * as the active rsx_backend.
 * Returns 0 on success, -1 on failure. */
int rsx_d3d12_backend_init(u32 width, u32 height, const char* title);

/* Shut down the D3D12 backend and release all resources. */
void rsx_d3d12_backend_shutdown(void);

/* Process Win32 messages. Returns 0 normally, -1 if window closed. */
int rsx_d3d12_backend_pump_messages(void);
/* Native-owner seam of the opt-in register-file engine (PS3RECOMP_D3D12_DRAW_ENGINE=1
 * with PS3RECOMP_RSX_NATIVE_OWNER=1), the contract of rsx_native_owner.h. -2 when the
 * engine or the owner seam is not active. */
#define RSX_D3D12_NATIVE_OWNER_ABI 1u   /* hosts test this to know the functions below exist */
struct rsx_native_owner; struct rsx_native_owner_v2;
int rsx_d3d12_backend_borrow_owner(struct rsx_native_owner* out);
int rsx_d3d12_backend_borrow_owner_v2(struct rsx_native_owner_v2* out);
/* Native image leases on the engine's existing colour and depth targets
 * (rsx_native_image.h). backend_surface is the engine handle the common
 * engine keeps for the guest surface; image points to rsx_native_d3d12_image,
 * retain before dereference. Native work acquires and hands back the resource
 * in its contract state: RENDER_TARGET for colour, DEPTH_WRITE for depth. */
#define RSX_D3D12_NATIVE_IMAGE_ABI 1u   /* hosts test this to know the three functions below exist */
struct rsx_native_image;
int rsx_d3d12_backend_borrow_image(uint64_t owner_lifetime, uint32_t backend_surface, struct rsx_native_image* out);
int rsx_d3d12_backend_validate_image_planes(void* user, uint64_t lifetime, uint64_t id, uint64_t generation, uint32_t required);
/* Submits what the engine has recorded (without waiting), with every borrowed
 * image moved into its contract state, and names that submission's fence
 * value: native work queued after it on the same queue runs after it. The
 * caller holds serialized FIFO consumption. 0, or -2 off, -3 stale/inactive,
 * -4 busy (a primitive is open), -6 failed. */
int rsx_d3d12_backend_native_boundary(uint64_t* original_ticket);
/* Reserved queue order (rsx_native_owner.h's rsx_native_queue_order) on the
 * engine's direct queue. submit's submit_info is an rsx_native_d3d12_submit
 * and its fence argument must be NULL: the native batch's own fence and value
 * travel in the struct. submit returns 0, the HRESULT of a failed signal, -1
 * for an invalid batch (the reservation is still released, as abandoned) or a
 * negative owner status. -2 when the owner seam is off. */
#define RSX_NATIVE_D3D12_SUBMIT_ABI 1u
typedef struct rsx_native_d3d12_submit {
    uint32_t abi_version, struct_size;
    void* const* lists;          /* ID3D12CommandList*, executed in this order */
    uint32_t list_count, reserved;
    void* signal_fence;          /* ID3D12Fence* signalled after the lists, or NULL */
    uint64_t signal_value;
} rsx_native_d3d12_submit;
struct rsx_native_queue_order;
int rsx_d3d12_backend_queue_order(struct rsx_native_queue_order* out);

/* Force a present (useful for debugging). */
void rsx_d3d12_backend_present(void);

#ifdef __cplusplus
}
#endif
#endif
