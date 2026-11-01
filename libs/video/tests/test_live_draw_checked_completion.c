/* Synthetic real-GPU check. No guest executable or media. */
#include <windows.h>
#include "live_draw_test_harness.h"
#define T "test_live_draw_checked_completion"
int main(void) {
    ldh_check("not ready before initialization",!rsx_live_draw_checked_ready());
    if(ldh_init(T))return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(),SW_HIDE);
    ldh_check("ready after initialization",rsx_live_draw_checked_ready()==1);
    ldh_check("empty submission completes",rsx_live_draw_flush_checked()==1);
    rsx_test_fp_mov_r0_col0(ldh_main+0x1000,RSX_TEST_FP_SWZ_IDENT);
    ldh_setup_quad(0,1,1,0,0);
    ldh_surface(0x8u|(1u<<8),0x10000,32,0x200000,32,8,8);
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE,0xFF000000);
    ldh_m(NV4097_CLEAR_SURFACE,0xF0);
    ldh_draw(0x1000,0x40,0,0,0x207);
    ldh_check("queued draw completes",rsx_live_draw_flush_checked()==1);
    static uint8_t rgb[8*8*3];u32 w=0,h=0;
    int read=ldh_read_rgb(0x10000,"checked_surface.ppm",rgb,8,8,&w,&h)==0;
    ldh_check("completed surface is readable",read && w==8 && h==8);
    unsigned white=0;if(read)for(unsigned i=0;i<64;i++)white+=rgb[i*3]>200;
    ldh_check("completed draw wrote every pixel",white==64);
    ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
    ldh_check("partial primitive cannot complete",rsx_live_draw_flush_checked()==0);
    ldh_m(NV4097_SET_BEGIN_END,0);
    ldh_check("complete boundary remains usable",rsx_live_draw_flush_checked()==1);
    rsx_live_draw_shutdown();
    ldh_check("shutdown revokes readiness",!rsx_live_draw_checked_ready());
    return ldh_done(T);
}
