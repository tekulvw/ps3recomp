/* CPU-only format contract; never creates a Metal device or submits work. */
#include "../rsx_metal_backend.m"
#include <assert.h>
static unsigned plane_queries;
int rsx_draw_engine_native_image_clear_planes(uint32_t handle,uint32_t required) {
    (void)handle;(void)required;++plane_queries;return -5;
}
int main(void) {
    uint8_t pixels[4]={9,9,9,9};
    rsx_metal_native_readback readback={.abi_version=1,.struct_size=sizeof(readback)};
    assert(rsx_metal_backend_read_native_image(&s_metal_owner,1,1,1,1,1,pixels,sizeof pixels,&readback)==-4);
    assert(!readback.gpu_completed&&!readback.bytes_written&&pixels[0]==9);
    assert(rsx_metal_backend_read_native_image(&s_metal_owner,1,1,1,0,1,pixels,sizeof pixels,&readback)==-1);
    assert(rsx_metal_backend_read_native_image(&s_metal_owner,1,1,1,1,1,pixels,3,&readback)==-1);
    assert(!readback.gpu_completed&&!readback.bytes_written&&pixels[3]==9);
    assert(rsx_metal_backend_validate_image_planes(&s_metal_owner,1,1,1,0)==-1);
    assert(rsx_metal_backend_validate_image_planes(&s_metal_owner,1,1,1,2)==-4);
    assert(!plane_queries); // Missing native lease must reject before decoder access.

    assert(metal_native_image_format(MTLPixelFormatRGBA8Unorm)==RSX_NATIVE_IMAGE_RGBA8);
    assert(metal_native_image_format(MTLPixelFormatBGRA8Unorm)==RSX_NATIVE_IMAGE_BGRA8);
    assert(metal_native_image_format(MTLPixelFormatDepth32Float)==RSX_NATIVE_IMAGE_D32F);
    assert(metal_native_image_format(MTLPixelFormatRGBA16Float)==RSX_NATIVE_IMAGE_RGBA16F);
    assert(metal_native_image_format(MTLPixelFormatRGBA32Float)==RSX_NATIVE_IMAGE_RGBA32F);
    assert(metal_native_image_format(MTLPixelFormatR32Float)==RSX_NATIVE_IMAGE_R32F);
    assert(metal_native_image_format(MTLPixelFormatDepth24Unorm_Stencil8)==RSX_NATIVE_IMAGE_D24S8);
    assert(metal_native_image_format(MTLPixelFormatDepth32Float_Stencil8)==RSX_NATIVE_IMAGE_D32S8);
    assert(!metal_native_image_format(MTLPixelFormatRGBA8Unorm_sRGB));
    assert(!metal_native_image_format(MTLPixelFormatInvalid));
    puts("PASS: borrowed image reports actual Metal format without depth reinterpretation");
}
