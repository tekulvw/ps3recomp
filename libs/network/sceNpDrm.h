/* NP DRM upgrade-license queries. These never fabricate an entitlement. */
#ifndef PS3RECOMP_SCE_NP_DRM_H
#define PS3RECOMP_SCE_NP_DRM_H
#include "ps3emu/ps3types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SCE_NP_DRM_ERROR_INVALID_PARAM     ((s32)0x80029502)
#define SCE_NP_DRM_ERROR_INTERNAL          ((s32)0x80029517)
#define SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND ((s32)0x80029521)
#define SCE_NP_DRM_ERROR_IO                ((s32)0x80029525)
/* One guest string argument, at most 47 bytes plus NUL. An installed unlock
 * EDAT currently returns INTERNAL: signature/decryption verification is not
 * implemented. Missing licenses return LICENSE_NOT_FOUND. */
s32 sceNpDrmVerifyUpgradeLicense(const char* content_id);
s32 sceNpDrmVerifyUpgradeLicense2(const char* content_id);
#ifdef __cplusplus
}
#endif
#endif
