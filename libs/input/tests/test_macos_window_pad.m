/* Real native view event dispatch -> cellPadGetData -> big-endian guest report. */
#import <AppKit/AppKit.h>
#include <assert.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "../../video/rsx_metal_backend.m"
#include "../cellPad.h"
#include "../../../runtime/ppu/ppu_memory.h"

uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a;(void)v;(void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a;return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }

static void key(NSView* view, NSEventType type, unsigned short code, NSEventModifierFlags mods) {
    NSEvent* e = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:mods
        timestamp:0 windowNumber:s_window.windowNumber context:nil characters:@"x"
        charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:code];
    (void)view;
    [NSApp sendEvent:e];
}
static uint16_t report(void) {
    usleep(6000);
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)0x1000) == CELL_OK);
    assert(vm_read32(0x1000) > 0);
    uint16_t lo=vm_read16(0x1000+4+CELL_PAD_BTN_OFFSET_DIGITAL1*2);
    uint16_t hi=vm_read16(0x1000+4+CELL_PAD_BTN_OFFSET_DIGITAL2*2);
    return lo | (hi << 8);
}
static void wait_key(NSWindow* window) {
    for (int i=0; i<200 && !window.isKeyWindow; i++) {
        NSEvent* e=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01] inMode:NSDefaultRunLoopMode dequeue:YES];
        if(e) [NSApp sendEvent:e];
    }
    assert(window.isKeyWindow);
}
int main(int argc, char** argv) { @autoreleasepool {
    vm_base = calloc(1, 0x10000); assert(vm_base);
    SDL_Joystick* virtual_pad=NULL;
    if (argc==2 && !strcmp(argv[1],"controller")) {
        assert(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)==0);
        int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
        assert(index>=0 && SDL_IsGameController(index));
        virtual_pad=SDL_JoystickOpen(index); assert(virtual_pad);
    }
    assert(cellPadInit(1) == CELL_OK);
    assert(cellPadSetPortSetting(0, CELL_PAD_SETTING_PRESS_ON) == CELL_OK);
    s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
    s_width=320; s_height=180;
    assert(create_window_impl("ps3recomp keyboard regression") == 0);
    [NSApp finishLaunching];
    wait_key(s_window);
    NSView* view = s_window.contentView;
    assert([view acceptsFirstResponder] && "native view must accept game keys");
    assert(s_window.firstResponder == view);
    assert(report()==0);
    if (virtual_pad) {
        key(view,NSEventTypeKeyDown,7,0);
        assert(ps3recomp_pad_macos_buttons()==CELL_PAD_CTRL_CROSS);
        assert(report()==0); /* attached SDL pad has priority */
        assert(SDL_JoystickSetVirtualButton(virtual_pad,SDL_CONTROLLER_BUTTON_B,1)==0);
        assert(report()==CELL_PAD_CTRL_CIRCLE);
        assert(SDL_JoystickSetVirtualButton(virtual_pad,SDL_CONTROLLER_BUTTON_B,0)==0);
        assert(report()==0);
        [s_window close]; assert(cellPadEnd()==CELL_OK);
        SDL_JoystickClose(virtual_pad); free(vm_base);
        puts("SDL controller overrides native keyboard fallback");
        return 0;
    }
    if (argc==2 && !strcmp(argv[1],"disabled")) {
        key(view,NSEventTypeKeyDown,7,0);
        assert(ps3recomp_pad_macos_buttons()==CELL_PAD_CTRL_CROSS);
        assert(report()==0);
        [s_window close]; assert(cellPadEnd()==CELL_OK); free(vm_base);
        puts("PAD_NO_KEYBOARD prevents native keyboard guest input");
        return 0;
    }
    key(view,NSEventTypeKeyDown,7,0); /* physical X */
    assert(report()==CELL_PAD_CTRL_CROSS);
    assert(vm_read16(0x1000+4+CELL_PAD_BTN_OFFSET_PRESS_CROSS*2)==255);
    key(view,NSEventTypeKeyDown,36,0); /* Return */
    assert(report()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_START));
    key(view,NSEventTypeKeyDown,7,0); /* repeat must not release */
    assert(report()==(CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_START));
    key(view,NSEventTypeKeyUp,7,0);
    assert(report()==CELL_PAD_CTRL_START);
    key(view,NSEventTypeKeyUp,36,0); assert(report()==0);
    key(view,NSEventTypeKeyDown,7,0); key(view,NSEventTypeKeyDown,6,0);
    key(view,NSEventTypeKeyUp,7,0); assert(report()==CELL_PAD_CTRL_SQUARE);
    key(view,NSEventTypeKeyUp,6,0); assert(report()==0);
    key(view,NSEventTypeKeyDown,36,0); key(view,NSEventTypeKeyDown,76,0);
    key(view,NSEventTypeKeyUp,36,0); assert(report()==CELL_PAD_CTRL_START);
    key(view,NSEventTypeKeyUp,76,0); assert(report()==0);
    key(view,NSEventTypeKeyDown,124,0); assert(report()==CELL_PAD_CTRL_RIGHT);
    key(view,NSEventTypeKeyUp,124,0); assert(report()==0);
    key(view,NSEventTypeKeyDown,125,0); assert(report()==CELL_PAD_CTRL_DOWN);
    key(view,NSEventTypeKeyUp,125,0); assert(report()==0);
    key(view,NSEventTypeKeyDown,126,0); assert(report()==CELL_PAD_CTRL_UP);
    key(view,NSEventTypeKeyUp,126,0); assert(report()==0);
    key(view,NSEventTypeKeyDown,123,0); assert(report()==CELL_PAD_CTRL_LEFT);
    assert(vm_read16(0x1000+4+CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X*2)==128);
    NSWindow* other=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,100,80) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    [other makeKeyAndOrderFront:nil]; wait_key(other); assert(report()==0);
    key(view,NSEventTypeKeyDown,7,0); assert(report()==0); /* unfocused */
    [s_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; wait_key(s_window); [s_window makeFirstResponder:view];
    key(view,NSEventTypeKeyDown,7,NSEventModifierFlagCommand); assert(report()==0);
    key(view,NSEventTypeKeyDown,7,0); assert(report()==CELL_PAD_CTRL_CROSS);
    [other close];
    [s_window orderOut:nil]; [s_window close]; assert(report()==0);
    assert(cellPadEnd()==CELL_OK); free(vm_base);
    puts("macOS native window keyboard -> guest pad checks passed");
} return 0; }
