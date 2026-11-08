/* Synthetic driver fault boundary, no Vulkan device or GPU calls. */
#include "../rsx_frontend_vulkan.c"
#include <assert.h>
#include <stdio.h>
static VkResult VKAPI_CALL fence_status(VkDevice device, VkFence fence) {
    (void)device;
    return fence == (VkFence)(uintptr_t)1 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}
int main(void) {
    frontend_vk c = {0}; c.vkGetFenceStatus = fence_status;
    c.slots[0].occupied = c.slots[1].occupied = 1;
    c.slots[0].fence = (VkFence)(uintptr_t)1; c.slots[1].fence = (VkFence)(uintptr_t)2;
    c.slots[0].ticket = (rsx_frontend_ticket){7,11}; c.slots[1].ticket = (rsx_frontend_ticket){7,12};
    rsx_frontend_event events[2]; uint32_t count = 0;
    assert(poll(&c, events, 2, &count) == RSX_FRONTEND_OK);
    assert(count == 1 && events[0].ticket.submission_id == 11 && events[0].succeeded && events[0].no_further_gpu_access);
    assert(!c.slots[0].occupied && c.slots[1].occupied && c.failed);
    count = 0;
    assert(poll(&c, events, 2, &count) == RSX_FRONTEND_DEVICE_FAILED && !count);
    assert(c.slots[1].occupied); /* lost/unknown ticket retains every lease */
    puts("PASS synthetic Vulkan driver fault: genuine terminal delivered once, failed slot retained, fault reported after valid event");
}
