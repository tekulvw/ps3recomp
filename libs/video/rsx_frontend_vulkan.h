#ifndef PS3RECOMP_RSX_FRONTEND_VULKAN_H
#define PS3RECOMP_RSX_FRONTEND_VULKAN_H
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <pthread.h>
#include "rsx_frontend.h"

/* Borrowed existing owner. begin_submit obtains its nonblocking recording gate;
 * end_submit releases it. Queue operations use the same external mutex as the
 * original renderer/present path. Calls remain serialized with its walker. */
typedef struct rsx_frontend_vulkan_owner {
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    PFN_vkGetDeviceProcAddr get_proc;
    pthread_mutex_t *queue_mutex;
    int (*begin_submit)(void *);
    void (*end_submit)(void *);
    void *user;
} rsx_frontend_vulkan_owner;
rsx_frontend_device *rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner *);
#endif
