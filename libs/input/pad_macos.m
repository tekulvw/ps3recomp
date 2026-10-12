#import "pad_macos.h"
#include "cellPad.h"
#include <stdatomic.h>

static _Atomic(uint16_t) s_keys;
static _Atomic(int) s_focused;
/* Keep physical keys separate so releasing an alias does not release the
 * button still held by its other key. Physical codes are layout-independent. */
static const struct { unsigned short code; uint16_t button; } s_map[] = {
    {7, CELL_PAD_CTRL_CROSS}, {6, CELL_PAD_CTRL_CROSS},
    {36, CELL_PAD_CTRL_START}, {76, CELL_PAD_CTRL_START},
    {123, CELL_PAD_CTRL_LEFT}, {124, CELL_PAD_CTRL_RIGHT},
    {125, CELL_PAD_CTRL_DOWN}, {126, CELL_PAD_CTRL_UP},
};
uint16_t ps3recomp_pad_macos_buttons(void) {
    if (!atomic_load(&s_focused)) return 0;
    uint16_t keys = atomic_load(&s_keys), buttons = 0;
    for (unsigned i=0; i<sizeof(s_map)/sizeof(s_map[0]); i++)
        if (keys & (1u << i)) buttons |= s_map[i].button;
    return buttons;
}
static uint16_t key_slot(unsigned short code) {
    for (unsigned i=0; i<sizeof(s_map)/sizeof(s_map[0]); i++)
        if (s_map[i].code == code) return (uint16_t)(1u << i);
    return 0;
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
    atomic_fetch_and(&s_keys, (uint16_t)~key_slot(event.keyCode));
}
- (void)flagsChanged:(NSEvent*)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) atomic_store(&s_keys, 0);
    [super flagsChanged:event];
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; [self releaseKeys]; }
@end
