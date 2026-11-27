/*
 * Malla - a channel set as a link: https://meshtastic.org/e/#<base64url>
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * After the '#' goes a ChannelSet protobuf (apponly.proto): the settings of
 * each channel in use, primary first, and the LoRa settings, in base64 with
 * the URL alphabet and no padding. That is what the phone apps show as a QR
 * and what they take. A "?add=true" at the end asks to add the channels
 * instead of replacing them; it is read past here, and the set replaces.
 */
#include "msh.h"

#include "pb_decode.h"
#include "pb_encode.h"

#include <stdlib.h>
#include <string.h>

#define URL_PREFIX "https://meshtastic.org/e/#"

bool msh_url_make(char *out, size_t cap)
{
    meshtastic_ChannelSet *cs = (meshtastic_ChannelSet *)msh_alloc(sizeof *cs);
    if (!cs) return false;
    msh_lock();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MSH_CHANNELS; i++) {
            const meshtastic_Channel *c = &MSH.ch[i];
            bool want = pass == 0 ? c->role == meshtastic_Channel_Role_PRIMARY
                                  : c->role == meshtastic_Channel_Role_SECONDARY;
            if (want && c->has_settings && cs->settings_count < 8) cs->settings[cs->settings_count++] = c->settings;
        }
    if (MSH.have_lora) {
        cs->has_lora_config = true;
        cs->lora_config = MSH.lora;
    }
    msh_unlock();
    uint8_t buf[meshtastic_ChannelSet_size + 8];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);
    bool ok = cs->settings_count && pb_encode(&os, meshtastic_ChannelSet_fields, cs);
    free(cs);
    if (!ok) return false;
    size_t pre = strlen(URL_PREFIX);
    if (cap <= pre + 4) return false;
    memcpy(out, URL_PREFIX, pre);
    return msh_b64url_encode(buf, (int)os.bytes_written, out + pre, (int)(cap - pre)) > 0;
}

bool msh_url_parse(const char *url, meshtastic_ChannelSet *out)
{
    const char *h = strchr(url, '#');
    const char *b = h ? h + 1 : url;           /* a bare base64 is taken too */
    char tmp[1100];
    size_t n = 0;
    for (; b[n] && b[n] != '?' && b[n] != '&' && n < sizeof tmp - 1; n++) tmp[n] = b[n];
    tmp[n] = 0;
    while (n && (tmp[n - 1] == ' ' || tmp[n - 1] == '\n' || tmp[n - 1] == '\r')) tmp[--n] = 0;
    uint8_t bin[meshtastic_ChannelSet_size + 8];
    int len = msh_b64_decode(tmp, bin, sizeof bin);
    if (len <= 0) return false;
    memset(out, 0, sizeof *out);
    pb_istream_t is = pb_istream_from_buffer(bin, (size_t)len);
    return pb_decode(&is, meshtastic_ChannelSet_fields, out) && out->settings_count > 0;
}
