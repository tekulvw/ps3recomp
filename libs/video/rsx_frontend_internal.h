#ifndef PS3RECOMP_RSX_FRONTEND_INTERNAL_H
#define PS3RECOMP_RSX_FRONTEND_INTERNAL_H
#include "rsx_frontend.h"
typedef struct rsx_frontend_ops {
    int (*caps)(void *, rsx_frontend_caps_desc *);
    int (*buffer_create)(void *, rsx_frontend_buffer, const rsx_frontend_buffer_desc *, rsx_frontend_buffer *);
    int (*buffer_release)(void *, rsx_frontend_buffer);
    int (*buffer_read)(void *, rsx_frontend_buffer, uint64_t, void *, uint64_t);
    int (*pipeline_create)(void *, const rsx_frontend_pipeline_desc *, rsx_frontend_pipeline *);
    int (*pipeline_release)(void *, rsx_frontend_pipeline);
    int (*submit)(void *, const rsx_frontend_frame *, rsx_frontend_ticket *);
    int (*poll)(void *, rsx_frontend_event *, uint32_t, uint32_t *);
    void (*close)(void *);
} rsx_frontend_ops;
rsx_frontend_device *rsx_frontend_wrap(const rsx_frontend_ops *, void *);
void rsx_frontend_close(rsx_frontend_device *);
uint64_t rsx_frontend_next_generation(void);
#ifdef __OBJC__
#import <Metal/Metal.h>
rsx_frontend_device *rsx_frontend_metal_create(id<MTLDevice>, id<MTLCommandQueue>, int (*ordering_ready)(void));
#endif
#endif
