/* Native Metal-window keyboard fallback for controller port zero. */
#ifndef PS3RECOMP_PAD_MACOS_H
#define PS3RECOMP_PAD_MACOS_H
#include <stdint.h>
/* Atomic snapshot; AppKit events write on the main thread, guest polls elsewhere. */
typedef struct {
    uint16_t buttons;
    uint8_t analog_lx, analog_ly, analog_rx, analog_ry;
} PS3RecompMacOSPadState;
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void);
uint16_t ps3recomp_pad_macos_buttons(void);
#ifdef __OBJC__
#import <AppKit/AppKit.h>
@interface PS3RecompPadView : NSView
@end
#endif
#endif
