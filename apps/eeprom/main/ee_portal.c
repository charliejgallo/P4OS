/*
 * P4OS - EEPROM: what the portal's page asks for, through the card.
 *
 * The page (web/eeprom.js) and the app share no memory; they meet in
 * <card>/eeprom/, in three small files (HANDOFF-APPS.md, section 7b):
 *
 *   chip.json     written here: the chosen chip and where it hangs, whether
 *                 the app is open, busy, or holds unwritten edits.
 *   pedido.json   written by the page: {"id":"…","op":"leer"} or
 *                 {"id":"…","op":"escribir","chip":"24LC256","archivo":"<version>.bin"}
 *                 or {"id":"…","op":"chip","chip":"25LC640"}. The app looks
 *                 for it every second, takes it (deletes it) and answers in
 *   estado.json   {"id":"…","op":"leer","estado":"trabajando"|"listo"|"error",
 *                 "fase":"…","progreso":0.42,"mensaje":"…","archivo":"…","copia":"…"}
 *
 * A write asked from the page is the same write as from the screen: a copy
 * of what the chip had goes to the versions first, only the pages that
 * differ are written, and everything is read back. The page asks before
 * sending it; here it is refused when the chip chosen on the board is not
 * the one the page meant, or when the board has edits not yet written.
 */
#include "ee.h"

static char s_id[40], s_op[16];

static bool path_of(const char *name, char *out, size_t len)
{
    const char *r = aos_hal_path_sd_root();
    if (!r || !r[0]) return false;
    snprintf(out, len, "%s/%s", r, EE_DIR);
    mkdir(out, 0755);
    snprintf(out, len, "%s/%s/%s", r, EE_DIR, name);
    return true;
}

static void put_file(const char *name, const char *body)
{
    char path[200], tmp[208];
    if (!path_of(name, path, sizeof path)) return;
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fputs(body, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        unlink(path);
        rename(tmp, path);
    } else unlink(tmp);
}

/* 'files': the versions this job made, once it ended. */
static void status(const char *state, const char *msg, float prog, bool files)
{
    char m[440], ph[64], id[60], op[32];
    ee_json_str(m, sizeof m, msg ? msg : "");
    ee_json_str(ph, sizeof ph, J.busy ? ee_phase_text(J.phase) : "");
    ee_json_str(id, sizeof id, s_id);
    ee_json_str(op, sizeof op, s_op);
    char body[900];
    snprintf(body, sizeof body,
             "{\"id\":%s,\"op\":%s,\"estado\":\"%s\",\"fase\":%s,\"progreso\":%.3f,\"mensaje\":%s,"
             "\"archivo\":\"%s\",\"copia\":\"%s\",\"chip\":\"%s\",\"t\":%u}\n",
             id, op, state, ph, (double)prog, m,
             !files ? "" : !strcmp(s_op, "escribir") ? J.saved2 : J.saved,
             files && !strcmp(s_op, "escribir") ? J.saved : "",
             ee_cur()->name, (unsigned)(aos_hal_uptime_ms() / 1000));
    put_file("estado.json", body);
}

static void fail(const char *msg)
{
    status("error", msg, 0, false);
    aos_hal_log("eeprom", "portal: %s", msg);
}

void ee_portal_chip(void)
{
    const ee_chip_t *c = ee_cur();
    char where[64];
    if (c->fam == EE_I2C) snprintf(where, sizeof where, "%s 0x%02X", S.conf.i2c_port, S.conf.i2c_addr);
    else if (c->fam == EE_MW) snprintf(where, sizeof where, "CS%d SK%d DI%d DO%d x%d", S.conf.mw[0], S.conf.mw[1], S.conf.mw[2], S.conf.mw[3], S.conf.mw_org);
    else if (S.conf.spi_cs >= 0) snprintf(where, sizeof where, "%s CS%d", S.conf.spi_port, S.conf.spi_cs);
    else snprintf(where, sizeof where, "%s", S.conf.spi_port);
    char body[400];
    snprintf(body, sizeof body,
             "{\"chip\":\"%s\",\"familia\":\"%s\",\"tamano\":%u,\"pagina\":%u,\"conexion\":\"%s\","
             "\"abierta\":%s,\"ocupada\":%s,\"cambios\":%u,\"t\":%u}\n",
             c->name, ee_fam_name(c->fam), (unsigned)c->size, c->page, where,
             U.root ? "true" : "false", J.busy ? "true" : "false", (unsigned)S.ndirty,
             (unsigned)(aos_hal_uptime_ms() / 1000));
    put_file("chip.json", body);
}

void ee_portal_progress(void)
{
    status("trabajando", "", J.total ? (float)J.done / (float)J.total : 0, false);
}

void ee_portal_done(void)
{
    status(J.ok ? "listo" : "error", J.msg, 1, true);
    ee_portal_chip();
}

/* The version 'file' of the chosen chip into the image, for a write. */
static bool load_version(const char *file)
{
    uint32_t size = 0;
    uint8_t *b = ee_ver_load(ee_cur()->name, file, &size);
    if (!b) return false;
    if (size != ee_cur()->size) { free(b); return false; }
    uint8_t *d = ee_alloc((size + 7) / 8);
    if (!d) { free(b); return false; }
    ee_img_free();
    S.img = b;
    S.dirty = d;
    memset(d, 0, (size + 7) / 8);
    S.size = size;
    S.valid = true;
    snprintf(S.img_chip, sizeof S.img_chip, "%s", ee_cur()->name);
    snprintf(S.img_from, sizeof S.img_from, "%s", _("del portal"));
    ee_sums_of(S.img, S.size, &S.sums);
    ee_diff_count();
    return true;
}

void ee_portal_poll(void)
{
    char path[200];
    if (!path_of("pedido.json", path, sizeof path)) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    char *js = ee_read_small(path, 1024);
    unlink(path);
    if (!js) return;
    char chip[24] = "", file[80] = "";
    s_id[0] = s_op[0] = 0;
    ee_json_get(js, "id", s_id, sizeof s_id);
    ee_json_get(js, "op", s_op, sizeof s_op);
    ee_json_get(js, "chip", chip, sizeof chip);
    ee_json_get(js, "archivo", file, sizeof file);
    free(js);
    char t[200];

    if (J.busy) { fail(_("La placa está ocupada con otro trabajo.")); return; }
    if (!strcmp(s_op, "chip")) {
        int i = ee_chip_find(chip);
        if (i < 0) { fail(_("No conozco ese chip.")); return; }
        S.conf.chip = i;
        ee_conf_save(&S.conf);
        S.vers_stale = true;
        S.det[0] = 0;
        S.sugg_chip = -1;
        ee_portal_chip();
        ee_rebuild();
        status("listo", "", 1, false);
        return;
    }
    if (chip[0] && strcmp(chip, ee_cur()->name)) {
        snprintf(t, sizeof t, _("En la placa está elegido el %s, no el %s."), ee_cur()->name, chip);
        fail(t);
        return;
    }
    if (S.ndirty) { fail(_("En la placa hay cambios sin escribir: escribilos o descartalos allá primero.")); return; }
    if (!strcmp(s_op, "leer")) {
        if (!ee_start_read(true)) { fail(_("No se pudo empezar a leer.")); return; }
        aos_ui_toast(_("El portal pidió leer el chip"), 2000);
        status("trabajando", "", 0, false);
        return;
    }
    if (!strcmp(s_op, "escribir")) {
        if (strchr(file, '/') || !file[0]) { fail(_("Falta la versión.")); return; }
        if (!load_version(file)) { fail(_("No se pudo leer esa versión, o no es del tamaño del chip.")); return; }
        if (!ee_start_write(WR_FULL, true)) { fail(_("No se pudo empezar a escribir.")); return; }
        aos_ui_toast(_("El portal pidió escribir el chip"), 2000);
        status("trabajando", "", 0, false);
        ee_rebuild();
        return;
    }
    fail(_("Pedido desconocido."));
}

void ee_portal_closed(void)
{
    lv_obj_t *keep = U.root;
    U.root = NULL;
    ee_portal_chip();
    U.root = keep;
}
