/*
 * BASTION - the common pack, loaded (see bs_art.h)
 */
#include "bs_art.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

static bool sheet(bs_art_t *a, bs_sheet_t *s, const char *name)
{
    if (s->data) return true;
    const bs_entry_t *e;
    uint8_t *d = (uint8_t *)bs_pak_get(&a->pak, name, &e);
    if (!d || e->type != PK_SHEET || !bs_sheet_bind(s, d, e->rawlen)) {
        bs_free(d);
        memset(s, 0, sizeof *s);
        return false;
    }
    return true;
}

static void drop(bs_sheet_t *s)
{
    bs_free(s->data);
    memset(s, 0, sizeof *s);
}

bool bs_art_open(bs_art_t *a, const char *path)
{
    memset(a, 0, sizeof *a);
    if (!bs_pak_open(&a->pak, path)) return false;
    a->open = true;
    static const char *const FONT[FONT_N] = { "font_s", "font_m", "font_l" };
    bool ok = true;
    for (int i = 0; i < FONT_N; i++) {
        const bs_entry_t *e;
        uint8_t *d = (uint8_t *)bs_pak_get(&a->pak, FONT[i], &e);
        if (!d || !bs_font_bind(&a->font[i], d, e->rawlen)) {
            bs_free(d);
            memset(&a->font[i], 0, sizeof a->font[i]);
            ok = false;
        }
    }
    ok &= sheet(a, &a->ui, "ui");
    ok &= sheet(a, &a->tw_icon, "tw_icons");
    ok &= sheet(a, &a->al_icon, "al_icons");
    if (!ok) aos_hal_log("bastion", "the pack lacks its fonts or icons");
    return ok;
}

void bs_art_battle_free(bs_art_t *a)
{
    for (int t = 0; t < TW_N; t++) {
        for (int k = 0; k < BS_TIERS; k++) {
            drop(&a->tw_base[t][k]);
            drop(&a->tw_head[t][k]);
        }
    }
    for (int i = 0; i < AL_N; i++) drop(&a->alien[i]);
    drop(&a->fort);
    drop(&a->missile);
    drop(&a->drone);
    a->aliens = 0;
}

void bs_art_close(bs_art_t *a)
{
    if (!a->open) return;
    bs_art_battle_free(a);
    drop(&a->ui);
    drop(&a->tw_icon);
    drop(&a->al_icon);
    for (int i = 0; i < FONT_N; i++) bs_free(a->font[i].data);
    bs_pak_close(&a->pak);
    memset(a, 0, sizeof *a);
}

bool bs_art_battle(bs_art_t *a, uint32_t alien_mask, const uint8_t *owned, int hue)
{
    char name[BS_NAME_LEN];
    a->missing = 0;
    /* the aliens are turned to the world's hue as they come in, so a
     * world of another hue reads them again */
    if (hue != a->hue) {
        for (int i = 0; i < AL_N; i++) drop(&a->alien[i]);
        a->aliens = 0;
    }
    a->hue = hue;
    for (int i = 0; i < AL_N; i++) {
        bool want = (alien_mask >> i) & 1;
        if (!want) {
            drop(&a->alien[i]);
            continue;
        }
        if (a->alien[i].data) continue;
        snprintf(name, sizeof name, "al_%s", BS_ALIEN[i].id);
        if (sheet(a, &a->alien[i], name)) bs_sheet_hue(&a->alien[i], hue);
        else a->missing++;
    }
    a->aliens = alien_mask;
    for (int t = 0; t < TW_N; t++) {
        for (int k = 0; k < BS_TIERS; k++) {
            if (!owned[t]) {
                drop(&a->tw_base[t][k]);
                drop(&a->tw_head[t][k]);
                continue;
            }
            snprintf(name, sizeof name, "tw_%s%d_b", BS_TOWER[t].id, k + 1);
            if (!sheet(a, &a->tw_base[t][k], name)) a->missing++;
            snprintf(name, sizeof name, "tw_%s%d_h", BS_TOWER[t].id, k + 1);
            if (!sheet(a, &a->tw_head[t][k], name)) a->missing++;
        }
    }
    if (!sheet(a, &a->fort, "fort")) a->missing++;
    if (!sheet(a, &a->missile, "missile")) a->missing++;
    if (!sheet(a, &a->drone, "drone")) a->missing++;
    if (a->missing) aos_hal_log("bastion", "%d sheets missing or out of memory", a->missing);
    return a->pak.fails == 0;
}
