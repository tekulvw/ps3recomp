#include "rsx_frontend_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct rsx_frontend_device { const rsx_frontend_ops *ops; void *context; };
static _Atomic uint64_t s_generation = 1;
uint64_t rsx_frontend_next_generation(void) {
    uint64_t current = atomic_load(&s_generation);
    while (current && current != UINT64_MAX) {
        if (atomic_compare_exchange_weak(&s_generation, &current, current + 1)) return current;
    }
    return 0;
}
rsx_frontend_device *rsx_frontend_wrap(const rsx_frontend_ops *ops, void *context) {
    if (!ops || !context) return NULL;
    rsx_frontend_device *device = malloc(sizeof(*device));
    if (device) *device = (rsx_frontend_device){ops, context};
    return device;
}
void rsx_frontend_close(rsx_frontend_device *device) {
    if (device) { device->ops->close(device->context); free(device); }
}
#define VALID(d) ((d) && (d)->struct_size == sizeof(*(d)) && (d)->abi_version == RSX_FRONTEND_ABI_VERSION)
int rsx_frontend_caps(rsx_frontend_device *device, rsx_frontend_caps_desc *desc) {
    return device && VALID(desc) ? device->ops->caps(device->context, desc) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_buffer_create(rsx_frontend_device *device, const rsx_frontend_buffer_desc *desc, rsx_frontend_buffer *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(desc) && out ? device->ops->buffer_create(device->context,
        (rsx_frontend_buffer){0}, desc, out) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_buffer_update(rsx_frontend_device *device, rsx_frontend_buffer previous,
    const rsx_frontend_buffer_desc *desc, rsx_frontend_buffer *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(desc) && out && previous.id ? device->ops->buffer_create(device->context,
        previous, desc, out) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_buffer_release(rsx_frontend_device *device, rsx_frontend_buffer ref) {
    return device ? device->ops->buffer_release(device->context, ref) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_buffer_read(rsx_frontend_device *device, rsx_frontend_buffer ref,
    uint64_t offset, void *out, uint64_t length) {
    return device && out && length ? device->ops->buffer_read(device->context, ref, offset, out, length) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_pipeline_create(rsx_frontend_device *device, const rsx_frontend_pipeline_desc *desc, rsx_frontend_pipeline *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(desc) && out ? device->ops->pipeline_create(device->context, desc, out) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_pipeline_release(rsx_frontend_device *device, rsx_frontend_pipeline ref) {
    return device ? device->ops->pipeline_release(device->context, ref) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_submit(rsx_frontend_device *device, const rsx_frontend_frame *frame, rsx_frontend_ticket *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(frame) && out ? device->ops->submit(device->context, frame, out) : RSX_FRONTEND_INVALID;
}
int rsx_frontend_poll(rsx_frontend_device *device, rsx_frontend_event *events, uint32_t capacity, uint32_t *count) {
    if (count) *count = 0;
    return device && events && capacity && capacity <= 64 && count ?
        device->ops->poll(device->context, events, capacity, count) : RSX_FRONTEND_INVALID;
}
