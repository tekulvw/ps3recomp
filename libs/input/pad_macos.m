#import "pad_macos.h"
#include "cellPad.h"
#include <stdatomic.h>

static _Atomic(uint32_t) s_keys;
static _Atomic(int) s_focused;
/* Keep physical keys separate so releasing an alias does not release the
 * button still held by its other key. Physical codes are layout-independent. */
static const struct { unsigned short code; uint16_t button; } s_map[] = {
    {7, CELL_PAD_CTRL_CROSS}, {6, CELL_PAD_CTRL_SQUARE},
    {8, CELL_PAD_CTRL_CIRCLE}, {9, CELL_PAD_CTRL_TRIANGLE},
    {36, CELL_PAD_CTRL_START}, {76, CELL_PAD_CTRL_START},
    {49, CELL_PAD_CTRL_SELECT},
    {12, CELL_PAD_CTRL_L1}, {14, CELL_PAD_CTRL_R1},
    {15, CELL_PAD_CTRL_L2}, {17, CELL_PAD_CTRL_R2},
    {3, CELL_PAD_CTRL_L3}, {5, CELL_PAD_CTRL_R3},
    {123, CELL_PAD_CTRL_LEFT}, {124, CELL_PAD_CTRL_RIGHT},
    {125, CELL_PAD_CTRL_DOWN}, {126, CELL_PAD_CTRL_UP},
    /* Stick keys have no digital-button alias: WASD left, IJKL right. */
    {34, 0}, {40, 0}, {38, 0}, {37, 0},
    {13, 0}, {1, 0}, {0, 0}, {2, 0},
};
_Static_assert(sizeof(s_map)/sizeof(s_map[0]) <= 32, "physical key mask capacity");
static uint32_t key_slot(unsigned short code) {
    for (unsigned i=0; i<sizeof(s_map)/sizeof(s_map[0]); i++)
        if (s_map[i].code == code) return 1u << i;
    return 0;
}
static uint8_t stick_axis(uint32_t keys, unsigned short negative, unsigned short positive) {
    int direction = !!(keys & key_slot(positive)) - !!(keys & key_slot(negative));
    return direction < 0 ? 0 : direction > 0 ? 255 : 128;
}
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void) {
    PS3RecompMacOSPadState state = {0, 128, 128, 128, 128};
    if (!atomic_load(&s_focused)) return state;
    /* All buttons and both sticks derive from one physical-key snapshot. */
    uint32_t keys = atomic_load(&s_keys);
    for (unsigned i=0; i<sizeof(s_map)/sizeof(s_map[0]); i++)
        if (keys & (1u << i)) state.buttons |= s_map[i].button;
    state.analog_lx = stick_axis(keys, 0, 2);   /* A / D */
    state.analog_ly = stick_axis(keys, 13, 1);  /* W / S */
    state.analog_rx = stick_axis(keys, 38, 37); /* J / L */
    state.analog_ry = stick_axis(keys, 34, 40); /* I / K */
    return state;
}
uint16_t ps3recomp_pad_macos_buttons(void) {
    return ps3recomp_pad_macos_state().buttons;
}
@implementation PS3RecompPadView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)releaseKeys {
    atomic_store(&s_focused, 0);
    atomic_store(&s_keys, 0);
}
- (BOOL)becomeFirstResponder {
    atomic_store(&s_focused, self.window.isKeyWindow);
    return YES;
}
- (BOOL)resignFirstResponder { [self releaseKeys]; return YES; }
- (void)windowBecameKey:(NSNotification*)note {
    (void)note;
    if (self.window.firstResponder == self) atomic_store(&s_focused, 1);
}
- (void)windowLostKey:(NSNotification*)note { (void)note; [self releaseKeys]; }
- (void)viewWillMoveToWindow:(NSWindow*)newWindow {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self releaseKeys];
    [super viewWillMoveToWindow:newWindow];
    if (newWindow) {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(windowBecameKey:) name:NSWindowDidBecomeKeyNotification object:newWindow];
        [nc addObserver:self selector:@selector(windowLostKey:) name:NSWindowDidResignKeyNotification object:newWindow];
        [nc addObserver:self selector:@selector(windowLostKey:) name:NSWindowWillCloseNotification object:newWindow];
    }
}
- (void)keyDown:(NSEvent*)event {
    /* Preserve application shortcuts; consume unbound game keys to avoid NSBeep. */
    if (event.modifierFlags & NSEventModifierFlagCommand) { atomic_store(&s_keys, 0); [super keyDown:event]; return; }
    if (!atomic_load(&s_focused)) return;
    atomic_fetch_or(&s_keys, key_slot(event.keyCode));
}
- (void)keyUp:(NSEvent*)event {
    atomic_fetch_and(&s_keys, (uint32_t)~key_slot(event.keyCode));
}
- (void)flagsChanged:(NSEvent*)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) atomic_store(&s_keys, 0);
    [super flagsChanged:event];
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; [self releaseKeys]; }
@end
