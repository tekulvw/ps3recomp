#ifndef PS3RECOMP_RSX_NATIVE_VULKAN_IMAGE_H
#define PS3RECOMP_RSX_NATIVE_VULKAN_IMAGE_H
#include "rsx_native_image.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RSX_NATIVE_VULKAN_IMAGE_ABI 1u
/* Backend2 rsx_native_image.image points to this retained immutable descriptor,
 * not a VkImage address. Actual2D optimal-tiling creation metadata is copied
 * from the allocation. Acquire/handback layout is GENERAL on the same owner
 * queue family; no transfer to another queue/device is authorized. Generation
 * callbacks cover both descriptor storage and native allocation. Numeric Vulkan
 * enums/flags keep this narrow provider header independent of Vulkan headers.
 */
typedef struct rsx_native_vulkan_image {
  uint32_t abi_version, struct_size;
  uint64_t image_bits;
  uint32_t format, usage, aspect, mip_count, layers, samples, queue_family,
      layout;
} rsx_native_vulkan_image;
#ifdef __cplusplus
}
#endif
#endif
