#ifndef PS3RECOMP_RSX_NATIVE_D3D12_IMAGE_H
#define PS3RECOMP_RSX_NATIVE_D3D12_IMAGE_H
#include "rsx_native_image.h"
#define RSX_NATIVE_D3D12_IMAGE_ABI 1u
/* Backend3 rsx_native_image.image points to this immutable retained descriptor.
 * resource is the genuine same-device ID3D12Resource. State enum values name
 * the whole allocation's source-tracked state at a checked original boundary.
 * Native commands must hand it back before Execute/Signal. No clear, resource,
 * device, queue, wait or source initialization is inferred from this metadata.
 */
typedef struct rsx_native_d3d12_image {
  uint32_t abi_version, struct_size;
  void *resource;
  uint32_t acquire_state, handback_state;
} rsx_native_d3d12_image;
#endif
