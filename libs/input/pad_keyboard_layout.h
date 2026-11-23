/* The keyboard-as-controller layout for pad port zero, shared by every host
 * that reads physical keys itself: the Linux SDL game window
 * (pad_sdl_keyboard.c) and the Windows game window (cellPad.c). The macOS
 * window pad (pad_macos.m) uses the same layout with AppKit key codes.
 *
 * Keys are named by their USB HID keyboard usage ID, which is a physical key
 * position (as printed on a US keyboard) rather than the character the active
 * text layout produces. SDL scancodes are these IDs; the Windows host converts
 * each to the virtual key at that position under the foreground layout.
 *
 *   X / Z / C / V       Cross / Square / Circle / Triangle
 *   Return, keypad Enter START
 *   Space               SELECT
 *   Q / E, R / T, F / G L1 / R1, L2 / R2, L3 / R3
 *   Arrow keys          D-pad (and nothing else)
 *   W / A / S / D       left stick, full deflection
 *   I / J / K / L       right stick, full deflection
 *
 * Opposing stick keys cancel to centre (128). A host builds one bit mask of
 * held keys, indexed by position in pad_keyboard_bindings[], passes it through
 * pad_keyboard_latch() and turns the result into a report with
 * pad_keyboard_report(). Header-only so each host's fixture builds with no
 * extra source file. */
#ifndef PS3RECOMP_PAD_KEYBOARD_LAYOUT_H
#define PS3RECOMP_PAD_KEYBOARD_LAYOUT_H
#include <stdint.h>
#include "cellPad.h"

/* USB HID keyboard usage IDs (HID Usage Tables, keyboard/keypad page 0x07). */
enum {
    PAD_KEY_A = 0x04, PAD_KEY_C = 0x06, PAD_KEY_D = 0x07, PAD_KEY_E = 0x08,
    PAD_KEY_F = 0x09, PAD_KEY_G = 0x0A, PAD_KEY_I = 0x0C, PAD_KEY_J = 0x0D,
    PAD_KEY_K = 0x0E, PAD_KEY_L = 0x0F, PAD_KEY_Q = 0x14, PAD_KEY_R = 0x15,
    PAD_KEY_S = 0x16, PAD_KEY_T = 0x17, PAD_KEY_V = 0x19, PAD_KEY_W = 0x1A,
    PAD_KEY_X = 0x1B, PAD_KEY_Z = 0x1D,
    PAD_KEY_RETURN = 0x28, PAD_KEY_SPACE = 0x2C,
    PAD_KEY_RIGHT = 0x4F, PAD_KEY_LEFT = 0x50, PAD_KEY_DOWN = 0x51, PAD_KEY_UP = 0x52,
    PAD_KEY_KP_ENTER = 0x58,
};

typedef struct {
    uint8_t usage;      /* PAD_KEY_* */
    uint16_t button;    /* CELL_PAD_CTRL_*; 0 for a stick key */
} PadKeyboardBinding;

static const PadKeyboardBinding pad_keyboard_bindings[] = {
    {PAD_KEY_X, CELL_PAD_CTRL_CROSS},     {PAD_KEY_Z, CELL_PAD_CTRL_SQUARE},
    {PAD_KEY_C, CELL_PAD_CTRL_CIRCLE},    {PAD_KEY_V, CELL_PAD_CTRL_TRIANGLE},
    {PAD_KEY_RETURN, CELL_PAD_CTRL_START}, {PAD_KEY_KP_ENTER, CELL_PAD_CTRL_START},
    {PAD_KEY_SPACE, CELL_PAD_CTRL_SELECT},
    {PAD_KEY_Q, CELL_PAD_CTRL_L1},        {PAD_KEY_E, CELL_PAD_CTRL_R1},
    {PAD_KEY_R, CELL_PAD_CTRL_L2},        {PAD_KEY_T, CELL_PAD_CTRL_R2},
    {PAD_KEY_F, CELL_PAD_CTRL_L3},        {PAD_KEY_G, CELL_PAD_CTRL_R3},
    {PAD_KEY_LEFT, CELL_PAD_CTRL_LEFT},   {PAD_KEY_RIGHT, CELL_PAD_CTRL_RIGHT},
    {PAD_KEY_DOWN, CELL_PAD_CTRL_DOWN},   {PAD_KEY_UP, CELL_PAD_CTRL_UP},
    {PAD_KEY_W, 0}, {PAD_KEY_S, 0}, {PAD_KEY_A, 0}, {PAD_KEY_D, 0},
    {PAD_KEY_I, 0}, {PAD_KEY_K, 0}, {PAD_KEY_J, 0}, {PAD_KEY_L, 0},
};
#define PAD_KEYBOARD_BINDING_COUNT (sizeof(pad_keyboard_bindings) / sizeof(pad_keyboard_bindings[0]))
_Static_assert(PAD_KEYBOARD_BINDING_COUNT <= 32, "held-key mask capacity");

typedef struct {
    uint16_t buttons;
    uint8_t analog_lx, analog_ly, analog_rx, analog_ry;
} PadKeyboardReport;

/* Bit for one usage in a held-key mask; 0 when the layout does not bind it. */
static inline uint32_t pad_keyboard_bit(uint8_t usage)
{
    for (unsigned i = 0; i < PAD_KEYBOARD_BINDING_COUNT; i++)
        if (pad_keyboard_bindings[i].usage == usage) return 1u << i;
    return 0;
}

/* Keys that count, given what is physically held and whether the game window
 * may take keyboard input now (focused, no dialog owning the keyboard). A key
 * still held when input becomes allowed again -- the Return that accepted a
 * dialog, a key held while switching back to the window -- stays ignored
 * until it is released, so it cannot arrive in the game as a fresh press.
 * `blocked` is the host's per-port memory of such keys, starting at 0. */
static inline uint32_t pad_keyboard_latch(uint32_t* blocked, uint32_t held, int allowed)
{
    if (!allowed) {
        *blocked = held;
        return 0;
    }
    *blocked &= held;   /* a release ends the block for that key */
    return held & ~*blocked;
}

static inline uint8_t pad_keyboard_axis(uint32_t keys, uint8_t negative, uint8_t positive)
{
    int direction = !!(keys & pad_keyboard_bit(positive)) - !!(keys & pad_keyboard_bit(negative));
    return direction < 0 ? 0 : direction > 0 ? 255 : 128;
}

/* Buttons and both sticks from one held-key mask. */
static inline PadKeyboardReport pad_keyboard_report(uint32_t keys)
{
    PadKeyboardReport report = {0, 128, 128, 128, 128};
    for (unsigned i = 0; i < PAD_KEYBOARD_BINDING_COUNT; i++)
        if (keys & (1u << i)) report.buttons |= pad_keyboard_bindings[i].button;
    report.analog_lx = pad_keyboard_axis(keys, PAD_KEY_A, PAD_KEY_D);
    report.analog_ly = pad_keyboard_axis(keys, PAD_KEY_W, PAD_KEY_S);
    report.analog_rx = pad_keyboard_axis(keys, PAD_KEY_J, PAD_KEY_L);
    report.analog_ry = pad_keyboard_axis(keys, PAD_KEY_I, PAD_KEY_K);
    return report;
}
#endif
