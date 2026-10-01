/*
 * P4OS - key names to USB HID usages (aos_hid_keys.c), platform-free: the
 * board's USB (aos_usb_p4.c) sends what this parses, and the simulator
 * (sim/usb_sim.c) parses the same names so a typo is caught on the desk too.
 *
 * A name is any number of modifiers and one key, joined by '+':
 * "cmd+shift+4", "ctrl+alt+t", "f5", "volup", "cmd+?". Case does not matter.
 * Modifiers alone ("shift", "cmd") press just those.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The modifier byte of a keyboard report */
#define AOS_HID_MOD_CTRL   0x01
#define AOS_HID_MOD_SHIFT  0x02
#define AOS_HID_MOD_ALT    0x04
#define AOS_HID_MOD_GUI    0x08

typedef struct {
    uint8_t  mods;          /* AOS_HID_MOD_* */
    uint8_t  key;           /* keyboard usage (page 0x07), 0 = none */
    uint16_t consumer;      /* consumer usage (page 0x0C), 0 = none; then mods/key are 0 */
} aos_hid_combo_t;

/* false for a name it does not know (out is then all zero). */
bool aos_hid_parse(const char *name, aos_hid_combo_t *out);

/* One ASCII character as a US keyboard types it: the usage and whether it
 * needs shift. false for what a US layout cannot type (accents, ñ). '\n' is
 * enter and '\t' tab. */
bool aos_hid_ascii(char c, uint8_t *key, bool *shift);

#ifdef __cplusplus
}
#endif
