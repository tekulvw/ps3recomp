/* CPU-only format contract; never creates a Metal device or submits work. */
#include "../rsx_metal_backend.m"
#include <assert.h>
int main(void) {
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
