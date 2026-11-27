/*
 * P4OS - The system services' glyphs in the status bar.
 *
 * The bar (aos_ui) cannot see the services, which live above it, so it asks
 * this provider once a second (aos_ui_status_provider) and each one sets
 * its glyph like an app working in the background would:
 *
 *   Zigbee, by what the C6's 802.15.4 radio is doing
 *     green    the board's network (hub), formed
 *     orange   the network open for devices to join (discovery)
 *     grey     the network starting
 *     red      the network could not start
 *     purple   emulating a device in someone else's network: joined;
 *              a paler purple while it looks for one or has left it
 *     cyan     a channel scan or a sniff (no network)
 *     none     the radio idle
 *   the Telegram bot, while it is on
 *     blue     online
 *     grey     connecting, or waiting for the network
 *     orange   no token, a token Telegram refuses, or another error
 */
#include "aos_bot.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"
#include "aos_zigbee.h"
#include "aos_154.h"
#include "aos_154_proto.h"

#define C_GREEN   0x34C759
#define C_ORANGE  0xFF9F0A
#define C_GREY    0x8E8E93
#define C_RED     0xFF453A
#define C_PURPLE  0xBF5AF2
#define C_PURPLE2 0x9A7DB8
#define C_CYAN    0x64D2FF
#define C_BLUE    0x2AABEE

static void zigbee(void)
{
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    uint32_t rgb = 0;
    if (z.emu) {
        aos_zigbee_emu_t e;
        aos_zigbee_emu_info(&e);
        rgb = e.state == AOS_ZBE_JOINED ? C_PURPLE : C_PURPLE2;
    } else if (z.state == AOS_ZB_FORMED) {
        rgb = aos_zigbee_permit_left() > 0 ? C_ORANGE : C_GREEN;
    } else if (z.state == AOS_ZB_STARTING) {
        rgb = C_GREY;
    } else if (z.state == AOS_ZB_FAILED) {
        rgb = C_RED;
    } else {
        aos_154_info_t r;
        aos_154_info(&r);
        if (r.state == AOS_154_SCANNING || r.state == AOS_154_SNIFFING) rgb = C_CYAN;
    }
    aos_ui_status_icon_set("sys.zigbee", rgb ? AOS_SYM_ZIGBEE : NULL, rgb);
}

static void bot(void)
{
    aos_bot_info_t b;
    aos_bot_info(&b);
    uint32_t rgb = 0;
    switch (b.state) {
    case AOS_BOT_OFF: break;
    case AOS_BOT_ONLINE: rgb = C_BLUE; break;
    case AOS_BOT_CONNECTING: case AOS_BOT_WAITING_NET: rgb = C_GREY; break;
    default: rgb = C_ORANGE;
    }
    aos_ui_status_icon_set("sys.bot", rgb ? AOS_SYM_ROBOT : NULL, rgb);
}

static void provider(void)
{
    zigbee();
    bot();
}

void aos_sys_status_start(void)
{
    aos_ui_status_provider(provider);
}
