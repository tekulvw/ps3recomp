/* Synthetic native key events -> production cellPad -> big-endian guest report.
 * No application, Metal device, or visible/key window is created. Only the
 * view's window focus source is doubled; event handlers and polling are real. */
#import <AppKit/AppKit.h>
#include <assert.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "../pad_macos.h"
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
u32 cellGcm_flip_request_count(void) { return 0; }   /* PAD_CLOCK=flips (#344) */

@interface PS3RecompPadView (SyntheticFocus)
- (void)windowBecameKey:(NSNotification*)note;
- (void)windowLostKey:(NSNotification*)note;
@end
@interface FocusSource : NSObject
@property BOOL isKeyWindow;
@property(weak) NSResponder* firstResponder;
@end
@implementation FocusSource
@end
@interface SyntheticPadView : PS3RecompPadView
@property(strong) FocusSource* focusSource;
@end
@implementation SyntheticPadView
- (NSWindow*)window { return (NSWindow*)self.focusSource; }
@end

static unsigned failures;
static void check(const char* label, unsigned got, unsigned want) {
    if (got != want) { fprintf(stderr,"FAIL %s: got %u, want %u\n",label,got,want); failures++; }
}
static void key(NSView* view, BOOL down, unsigned short code) {
    NSEvent* e=[NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:code];
    if (down) [view keyDown:e]; else [view keyUp:e];
}
static uint16_t word(unsigned offset) { return vm_read16(0x1000+4+offset*2); }
static void report(const char* label, uint16_t buttons, unsigned lx, unsigned ly, unsigned rx, unsigned ry) {
    usleep(6000);
    assert(cellPadGetData(0,(CellPadData*)(uintptr_t)0x1000)==CELL_OK);
    assert(vm_read32(0x1000)>0);
    check(label,word(CELL_PAD_BTN_OFFSET_DIGITAL1)|(word(CELL_PAD_BTN_OFFSET_DIGITAL2)<<8),buttons);
    check("left X",word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X),lx);
    check("left Y",word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y),ly);
    check("right X",word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X),rx);
    check("right Y",word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y),ry);
}
int main(int argc, char** argv) { @autoreleasepool {
    vm_base=calloc(1,0x10000); assert(vm_base);
    SDL_Joystick* controller=NULL;
    BOOL physical=argc==2 && !strcmp(argv[1],"controller");
    BOOL disabled=argc==2 && !strcmp(argv[1],"disabled");
    if (physical) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
        assert(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)==0);
        int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
        assert(index>=0 && SDL_IsGameController(index));
        controller=SDL_JoystickOpen(index); assert(controller);
    }
    assert(cellPadInit(1)==CELL_OK);
    assert(cellPadSetPortSetting(0,CELL_PAD_SETTING_PRESS_ON)==CELL_OK);
    SyntheticPadView* view=[[SyntheticPadView alloc] initWithFrame:NSZeroRect];
    view.focusSource=[FocusSource new];
    view.focusSource.isKeyWindow=YES; view.focusSource.firstResponder=view;
    assert([view becomeFirstResponder]);
    if (physical || disabled) {
        key(view,YES,7); key(view,YES,13); key(view,YES,34);
        check("native Cross before port arbitration",ps3recomp_pad_macos_buttons(),CELL_PAD_CTRL_CROSS);
        /* SDL's existing centered-axis conversion produces 127. */
        unsigned center=physical ? 127 : 0; /* disabled fallback leaves the vacant port zeroed */
        report("keyboard fallback suppressed",0,center,center,center,center);
        if (physical) {
            assert(SDL_JoystickSetVirtualButton(controller,SDL_CONTROLLER_BUTTON_B,1)==0);
            assert(SDL_JoystickSetVirtualAxis(controller,SDL_CONTROLLER_AXIS_RIGHTX,32767)==0);
            report("physical controller still drives port",CELL_PAD_CTRL_CIRCLE,127,127,255,127);
        }
    } else {
        report("neutral",0,128,128,128,128);
        const struct { unsigned short code; unsigned lx,ly,rx,ry; } sticks[]={
            {13,128,128,128,0}, {1,128,128,128,255}, {0,128,128,0,128}, {2,128,128,255,128},
            {34,128,0,128,128}, {40,128,255,128,128}, {38,0,128,128,128}, {37,255,128,128,128},
        };
        for (unsigned i=0;i<sizeof(sticks)/sizeof(sticks[0]);i++) {
            key(view,YES,sticks[i].code);
            report("stick direction",0,sticks[i].lx,sticks[i].ly,sticks[i].rx,sticks[i].ry);
            key(view,NO,sticks[i].code); report("stick released",0,128,128,128,128);
        }
        key(view,YES,13); key(view,YES,2); key(view,YES,40); key(view,YES,38);
        report("independent simultaneous sticks",0,0,255,255,0);
        key(view,YES,1); key(view,YES,0); key(view,YES,34); key(view,YES,37);
        report("opposing directions neutral",0,128,128,128,128);
        key(view,NO,1); key(view,NO,0); key(view,NO,34); key(view,NO,37);
        report("release opposing keys restores held directions",0,0,255,255,0);
        [view windowLostKey:nil]; report("focus clears sticks",0,128,128,128,128);
        key(view,YES,13); report("unfocused keys ignored",0,128,128,128,128);
        [view windowBecameKey:nil]; report("focus return stays released",0,128,128,128,128);
        const struct { unsigned short code; uint16_t button; int pressure; } buttons[]={
            {7,CELL_PAD_CTRL_CROSS,CELL_PAD_BTN_OFFSET_PRESS_CROSS},
            {6,CELL_PAD_CTRL_SQUARE,CELL_PAD_BTN_OFFSET_PRESS_SQUARE},
            {8,CELL_PAD_CTRL_CIRCLE,CELL_PAD_BTN_OFFSET_PRESS_CIRCLE},
            {9,CELL_PAD_CTRL_TRIANGLE,CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE},
            {36,CELL_PAD_CTRL_START,-1},{76,CELL_PAD_CTRL_START,-1},{49,CELL_PAD_CTRL_SELECT,-1},
            {12,CELL_PAD_CTRL_L1,CELL_PAD_BTN_OFFSET_PRESS_L1},{14,CELL_PAD_CTRL_R1,CELL_PAD_BTN_OFFSET_PRESS_R1},
            {15,CELL_PAD_CTRL_L2,CELL_PAD_BTN_OFFSET_PRESS_L2},{17,CELL_PAD_CTRL_R2,CELL_PAD_BTN_OFFSET_PRESS_R2},
            {3,CELL_PAD_CTRL_L3,-1},{5,CELL_PAD_CTRL_R3,-1},
            {123,CELL_PAD_CTRL_LEFT,CELL_PAD_BTN_OFFSET_PRESS_LEFT},{124,CELL_PAD_CTRL_RIGHT,CELL_PAD_BTN_OFFSET_PRESS_RIGHT},
            {125,CELL_PAD_CTRL_DOWN,CELL_PAD_BTN_OFFSET_PRESS_DOWN},{126,CELL_PAD_CTRL_UP,CELL_PAD_BTN_OFFSET_PRESS_UP},
        };
        for (unsigned i=0;i<sizeof(buttons)/sizeof(buttons[0]);i++) {
            key(view,YES,buttons[i].code);
            report("digital button with centered sticks",buttons[i].button,128,128,128,128);
            if(buttons[i].pressure>=0) check("digital pressure",word(buttons[i].pressure),255);
            key(view,NO,buttons[i].code); report("button released",0,128,128,128,128);
        }
        key(view,YES,36); key(view,YES,76); key(view,NO,36);
        report("keypad Enter survives Return release",CELL_PAD_CTRL_START,128,128,128,128);
        key(view,NO,76);
        key(view,YES,7); key(view,YES,6); key(view,YES,8); key(view,YES,9);
        key(view,YES,12); key(view,YES,14); key(view,YES,15); key(view,YES,17);
        key(view,YES,3); key(view,YES,5); key(view,YES,49); key(view,YES,36);
        report("all non-D-pad buttons held",0xFF0F,128,128,128,128);
        key(view,YES,7); report("repeat keeps held buttons",0xFF0F,128,128,128,128);
        [view resignFirstResponder]; report("responder loss releases buttons",0,128,128,128,128);
        [view becomeFirstResponder]; report("responder return stays released",0,128,128,128,128);
    }
    [view resignFirstResponder];
    view.focusSource=nil; /* NSView teardown must see its actual absent window. */
    assert(cellPadEnd()==CELL_OK);
    if(controller) SDL_JoystickClose(controller);
    free(vm_base);
    if (failures) fprintf(stderr,"%u native keyboard layout assertions failed\n",failures);
    else puts("native keyboard layout guest-report checks passed");
    return failures ? 1 : 0;
} }
