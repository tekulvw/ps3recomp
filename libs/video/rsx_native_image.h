#ifndef PS3RECOMP_RSX_NATIVE_IMAGE_H
#define PS3RECOMP_RSX_NATIVE_IMAGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Narrow default-off existing image owner metadata; no rendering commands.
 * Enabled with PS3RECOMP_RSX_NATIVE_OWNER=1. A getter snapshot is unretained:
 * retain(owner,id,generation) must succeed before native image dereference.
 * The original owner pins a retired slot until the final release; an older
 * retained image may survive retirement but validate must reject it for new
 * encoding. Validate runs under the actual owner's native queue lease.
 * Callback release is non-GPU-waiting and never retries host shutdown. */
#define RSX_NATIVE_IMAGE_ABI 1u
#define RSX_NATIVE_IMAGE_RGBA8 1u
#define RSX_NATIVE_IMAGE_BGRA8 3u
#define RSX_NATIVE_IMAGE_D32F 2u
#define RSX_NATIVE_IMAGE_RGBA16F 11u
#define RSX_NATIVE_IMAGE_RGBA32F 12u
#define RSX_NATIVE_IMAGE_R32F 13u
#define RSX_NATIVE_IMAGE_D24S8 15u
#define RSX_NATIVE_IMAGE_D32S8 16u
typedef struct rsx_native_image {
 uint32_t abi_version,struct_size,backend,format,width,height,reserved,reserved1;
 uint64_t owner_lifetime,resource_id,resource_generation;
 void *image,*userdata;
 int (*retain)(void *,uint64_t,uint64_t,uint64_t);
 void (*release)(void *,uint64_t,uint64_t,uint64_t);
 int (*validate)(void *,uint64_t,uint64_t,uint64_t);
} rsx_native_image;
#ifdef __cplusplus
}
#endif
#endif
