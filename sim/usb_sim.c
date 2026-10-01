/*
 * P4OS simulator - the P4OS additions to the USB block of aos_hal.h (the
 * Macro pad). The rest of the block is in hal_sim.c: the keyboard is
 * "ready" as soon as the mode is KEYS, and every call only prints.
 *
 * Nothing here, or there, ever reaches the Mac's own keyboard or mouse: the
 * simulator logs what the board would send and the app shows it on screen.
 * The key names go through the same parser as the board's
 * (components/aos_hal/aos_hid_keys.c), so a name the board would refuse is
 * refused here too.
 */
#include "aos_hal.h"
#include "aos_hid_keys.h"

#include <stdio.h>

bool aos_hal_usb_key_valid(const char *name)
{
    aos_hid_combo_t c;
    return aos_hid_parse(name, &c);
}

bool aos_hal_usb_mouse_hold(int buttons)
{
    if (!aos_hal_usb_keys_ready()) return false;
    printf("[usb] mouse hold %d (log only, nothing reaches the Mac)\n", buttons & 3);
    return true;
}
