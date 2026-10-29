/*
 * P4OS - Programmer: the ESP32 family, through esp-serial-flasher.
 *
 * The port layer (esp_loader_port_ops_t) is aos_io, nothing of ESP-IDF, so
 * the same file runs in the simulator against tools/fake_esp_rom.py:
 *   read/write         aos_io_uart_read/_write, with the library's timeouts;
 *                      a cancel makes them fail, which unwinds any command
 *   enter_bootloader   esptool's classic sequence on aos_io_uart_lines: EN
 *                      low with BOOT high, then BOOT low and EN released in
 *                      one step, then BOOT released. It is the only order
 *                      that also works through the usual two-transistor
 *                      auto-reset circuit (RTS/DTR in the simulator), where
 *                      both lines low at once cannot exist.
 *   reset_target       EN low, then released with BOOT high: the program runs
 *   baud               aos_io_uart_set_baud
 * Without EN/BOOT on the port (no "en=/boot=" in modules.txt) the job asks
 * for the target to be put in download mode by hand and keeps trying SYNC
 * for a few seconds.
 *
 * A flash job: open the port at 115200, connect and upload the flasher stub
 * (the ROM itself if the stub does not come up, or if asked), read the chip
 * (name, revision, MAC, flash size, security), refuse a build for another
 * chip or a flash that is encrypted, go to the fast speed (460800 by
 * default) and check the link with a register read, falling back to 115200
 * with a new reset if it does not answer; then every file: FLASH_BEGIN
 * (which erases), the data in blocks read from the card, and the MD5 the
 * target computes of what it has against the one of what was sent; at the
 * end a hard reset so the new program runs, and the port closes, which lets
 * EN and BOOT go.
 *
 * A read job (a dump) connects the same way and reads the flash in 16 KB
 * pieces into the core's dump file. With the stub each piece comes with the
 * MD5 the target computed of it, which the library checks; the ROM reads
 * 64 bytes per command and checks nothing, so it is said. An erase job is
 * the stub's ERASE_FLASH (or the ROM's FLASH_BEGIN over the whole size).
 *
 * Blocks: 16 KB with the stub at 460800 and up, 4 KB below (a 16 KB block at
 * 115200 takes 1.4 s on the wire and the library gives each block 1 s), and
 * 1 KB for the ROM, esptool's sizes. Files are padded with 0xFF to a
 * multiple of four, as esptool does; the MD5 covers the padded image.
 *
 * Not here yet: compressed writes (FLASH_DEFL_*, the stub supports them but
 * it needs a deflate that the simulator also has), downloads from a URL and
 * the RFC2217 bridge (docs/plan/PROGRAMADOR.md, F4).
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if __has_include("esp_loader.h")
#define HAVE_ESF 1
#include "esp_loader.h"
#include "esp_loader_io.h"
#include "esp_loader_stubs.h"
#else
#define HAVE_ESF 0
#endif

#define BAUD_FAST   460800
#define BLOCK_MAX   16384

#if HAVE_ESF

/* -------------------------------------------------------------------------- */
/* The port: esp-serial-flasher over aos_io                                    */
/* -------------------------------------------------------------------------- */

#define RESET_HOLD_MS 100
#define BOOT_HOLD_MS  50

typedef struct {
    esp_loader_port_t port;         /* first, for container_of */
    aos_io_uart_t *u;
    uint64_t deadline;
    bool lines;
    bool jtag;                      /* the target's own USB-Serial-JTAG */
} fl_port_t;

static esp_loader_error_t p_read(esp_loader_port_t *port, uint8_t *data, uint16_t size, uint32_t timeout)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    uint64_t end = aos_hal_uptime_ms() + timeout;
    int got = 0;
    for (;;) {
        if (fl_cancel) return ESP_LOADER_ERROR_FAIL;
        uint64_t now = aos_hal_uptime_ms();
        int left = now < end ? (int)(end - now) : 0;
        int n = aos_io_uart_read(p->u, data + got, size - got, left > 50 ? 50 : left);
        if (n < 0) return ESP_LOADER_ERROR_FAIL;
        got += n;
        if (got >= size) return ESP_LOADER_SUCCESS;
        if (!left) return ESP_LOADER_ERROR_TIMEOUT;
    }
}

static esp_loader_error_t p_write(esp_loader_port_t *port, const uint8_t *data, uint16_t size, uint32_t timeout)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    uint64_t end = aos_hal_uptime_ms() + (timeout < 1000 ? 1000 : timeout);
    int done = 0;
    while (done < size) {
        if (fl_cancel) return ESP_LOADER_ERROR_FAIL;
        int n = aos_io_uart_write(p->u, data + done, size - done);
        if (n < 0) return ESP_LOADER_ERROR_FAIL;
        done += n;
        if (done < size) {
            if (aos_hal_uptime_ms() > end) return ESP_LOADER_ERROR_TIMEOUT;
            aos_hal_sleep_ms(2);
        }
    }
    return ESP_LOADER_SUCCESS;
}

static void p_enter_bootloader(esp_loader_port_t *port)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    if (p->jtag) {
        /* esptool's USBJTAGSerialReset, step for step: the chip's USB logic
         * resets it when RTS rises with DTR low, into the ROM if DTR was
         * high just before. It goes through (1,1), never (0,0). */
        aos_io_uart_dtr_rts(p->u, false, false);
        aos_hal_sleep_ms(100);
        aos_io_uart_dtr_rts(p->u, true, false);
        aos_hal_sleep_ms(100);
        aos_io_uart_dtr_rts(p->u, true, true);
        aos_io_uart_dtr_rts(p->u, false, true);
        aos_hal_sleep_ms(100);
        aos_io_uart_dtr_rts(p->u, false, false);
        return;
    }
    if (!p->lines) return;
    aos_io_uart_lines(p->u, 0, 1);      /* held in reset, GPIO0 high */
    aos_hal_sleep_ms(RESET_HOLD_MS);
    aos_io_uart_lines(p->u, 1, 0);      /* GPIO0 low, then EN released: the ROM samples it low */
    aos_hal_sleep_ms(BOOT_HOLD_MS);
    aos_io_uart_lines(p->u, -1, 1);
}

static void p_reset_target(esp_loader_port_t *port)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    if (p->jtag) {
        /* RTS alone: a reset with DTR low, into the application */
        aos_io_uart_dtr_rts(p->u, false, true);
        aos_hal_sleep_ms(200);
        aos_io_uart_dtr_rts(p->u, false, false);
        return;
    }
    if (!p->lines) return;
    aos_io_uart_lines(p->u, 0, 1);
    aos_hal_sleep_ms(RESET_HOLD_MS);
    aos_io_uart_lines(p->u, 1, 1);
}

/* esp-serial-flasher gives an erase 10 s per MB; esptool gives it 30. A
 * 16 MB flash erasing 4 KB sectors at ~45 ms each needs ~11.5 s per MB.
 * Above 3 s a timeout is one of the library's per-MB ones (erase, MD5,
 * loading to RAM), never one that waits for an answer, so those are
 * tripled: esptool's margin. */
static void p_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    if (ms > 3000) ms *= 3;
    container_of(port, fl_port_t, port)->deadline = aos_hal_uptime_ms() + ms;
}

static uint32_t p_remaining_time(esp_loader_port_t *port)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    uint64_t now = aos_hal_uptime_ms();
    return p->deadline > now ? (uint32_t)(p->deadline - now) : 0;
}

static void p_delay_ms(esp_loader_port_t *port, uint32_t ms) { (void)port; aos_hal_sleep_ms(ms); }

static void p_log(esp_loader_port_t *port, esp_loader_log_level_t level, const char *fmt, va_list args)
{
    (void)port;
    /* the chip-detection probe: GET_SECURITY_INFO is refused by the ESP32's
     * and the ESP8266's ROMs, which the library reports as an error */
    if (fl_st.phase == AOS_FLASHER_CONNECTING && !strncmp(fmt, "Protocol error", 14)) return;
    if (fl_cancel) return;           /* a cancel fails the command in flight: its retries are noise */
    fl_log_v(level <= ESP_LOADER_LOG_LEVEL_ERROR ? L_ERR : level == ESP_LOADER_LOG_LEVEL_WARN ? L_WARN : L_INFO, fmt, args);
}

static esp_loader_error_t p_baud(esp_loader_port_t *port, uint32_t rate)
{
    fl_port_t *p = container_of(port, fl_port_t, port);
    return aos_io_uart_set_baud(p->u, rate) ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static const esp_loader_port_ops_t PORT_OPS = {
    .enter_bootloader = p_enter_bootloader,
    .reset_target = p_reset_target,
    .start_timer = p_start_timer,
    .remaining_time = p_remaining_time,
    .delay_ms = p_delay_ms,
    .log = p_log,
    .change_transmission_rate = p_baud,
    .write = p_write,
    .read = p_read,
};

/* -------------------------------------------------------------------------- */
/* The job                                                                     */
/* -------------------------------------------------------------------------- */

static const char *const CHIP_IDF[ESP_MAX_CHIP] = {
    [ESP8266_CHIP] = "esp8266", [ESP32_CHIP] = "esp32", [ESP32S2_CHIP] = "esp32s2",
    [ESP32C3_CHIP] = "esp32c3", [ESP32S3_CHIP] = "esp32s3", [ESP32C2_CHIP] = "esp32c2",
    [ESP32C5_CHIP] = "esp32c5", [ESP32H2_CHIP] = "esp32h2", [ESP32C6_CHIP] = "esp32c6",
    [ESP32P4_CHIP] = "esp32p4", [ESP32C61_CHIP] = "esp32c61", [ESP32S31_CHIP] = "esp32s31",
    [ESP32H21_CHIP] = "esp32h21", [ESP32H4_CHIP] = "esp32h4",
};

static const char *err_text(esp_loader_error_t e)
{
    switch (e) {
    case ESP_LOADER_SUCCESS: return _("bien");
    case ESP_LOADER_ERROR_TIMEOUT: return _("no respondió a tiempo");
    case ESP_LOADER_ERROR_IMAGE_SIZE: return _("la imagen no entra en la flash");
    case ESP_LOADER_ERROR_INVALID_MD5: return _("el MD5 no coincide");
    case ESP_LOADER_ERROR_INVALID_PARAM: return _("parámetro inválido");
    case ESP_LOADER_ERROR_INVALID_TARGET: return _("chip desconocido");
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP: return _("chip no soportado");
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC: return _("el chip no lo soporta");
    case ESP_LOADER_ERROR_INVALID_RESPONSE: return _("respuesta inválida");
    default: return _("falló");
    }
}

/* Flash encryption, as esptool decides it: an odd number of bits set in
 * FLASH_CRYPT_CNT. esp-serial-flasher (2.1.0, and its main branch on
 * 2026-10-15) counts the bits of the key purposes instead, so an ESP32-P4
 * v3.1 with nothing encrypted came out "encrypted" and could not be
 * flashed. Its GET_SECURITY_INFO is asked again here, the reply laid out as
 * the library's protocol.h has it (an ESP32-S2's lacks the last 8 bytes). */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint8_t  flash_crypt_cnt;
    uint8_t  key_purposes[7];
    uint32_t chip_id;
    uint32_t eco_version;
} sec_reply_t;
extern esp_loader_error_t loader_get_security_info_cmd(esp_loader_t *loader, sec_reply_t *reply, uint32_t *got);

typedef struct {
    esp_loader_t L;
    fl_port_t P;
    aos_io_uart_t *u;
    uint8_t *buf;
} ctx_t;

/* ESP32-P4 v3.1 and v3.2 (ECO6, ECO7) come out of reset with their flash
 * unpowered: the default went from 1.8 V to 3.3 V, so it stays off until
 * someone powers it at the voltage the eFuses say. esptool does it right
 * after connecting (esptool 5.3.1, targets/esp32p4.py, power_on_flash);
 * esp-serial-flasher does not yet (its issue 184), and an ESP32-P4-Nano
 * (v3.1) answered, read its MAC and then hung on the first erase. The same
 * register writes, through the ROM, before the stub goes up. */
#define P4_EFUSE_RD_REPEAT_DATA1    0x5012D034u
#define P4_EFUSE_DL_XPD_ON          (1u << 16)
#define P4_LP_SYS_XPD_PAD_GROUP     0x5011010Cu
#define P4_PMU_EXT_LDO_P0_0P1A      0x501151B8u
#define P4_PMU_EXT_LDO_P0_0P1A_ANA  0x501151BCu
#define P4_PMU_DATE                 0x501153FCu
#define P4_ANA_0P1A_EN_CUR_LIM_0    (1u << 27)
#define P4_0P1A_TARGET0_0           (0xFFu << 23)
#define P4_0P1A_FORCE_TIEH_SEL_0    (1u << 7)

static esp_loader_error_t reg_or(esp_loader_t *l, uint32_t a, uint32_t set, uint32_t clear)
{
    uint32_t v;
    esp_loader_error_t e = esp_loader_read_register(l, a, &v);
    if (e != ESP_LOADER_SUCCESS) return e;
    return esp_loader_write_register(l, a, (v | set) & ~clear);
}

static void p4_power_on_flash(esp_loader_t *l)
{
    uint16_t rev = 0xFFFF;
    if (esp_loader_get_chip_revision(l, &rev) != ESP_LOADER_SUCCESS || (rev != 301 && rev != 302)) return;
    esp_loader_target_security_info_t si;
    if (esp_loader_get_security_info(l, &si) == ESP_LOADER_SUCCESS && si.secure_download_mode_enabled) return;
    uint32_t v = 0;
    if (rev == 302 && esp_loader_read_register(l, P4_EFUSE_RD_REPEAT_DATA1, &v) == ESP_LOADER_SUCCESS &&
        (v & P4_EFUSE_DL_XPD_ON)) {
        /* the ROM powered it already; only release its force-on */
        esp_loader_write_register(l, P4_PMU_DATE, 0);
        return;
    }
    esp_loader_error_t e = esp_loader_write_register(l, P4_LP_SYS_XPD_PAD_GROUP, 1);
    aos_hal_sleep_ms(10);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A_ANA, P4_ANA_0P1A_EN_CUR_LIM_0, 0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, P4_0P1A_FORCE_TIEH_SEL_0, 0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_DATE, 3, 0);
    aos_hal_sleep_ms(1);                                    /* esptool: 50 us */
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A_ANA, 0, P4_ANA_0P1A_EN_CUR_LIM_0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, 0, P4_0P1A_TARGET0_0);
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, 0x80, 0);     /* the eFuses' voltage */
    if (e == ESP_LOADER_SUCCESS) e = reg_or(l, P4_PMU_EXT_LDO_P0_0P1A, 0, P4_0P1A_FORCE_TIEH_SEL_0);
    aos_hal_sleep_ms(2);                                    /* esptool: 1.8 ms */
    aos_hal_log("flasher", "ESP32-P4 v%u.%u: flash powered on (%s)", rev / 100, rev % 100,
                e == ESP_LOADER_SUCCESS ? "ok" : "a register write failed");
}

/* The stub the library would pick, with the P4's flash powered first. The
 * library's own provider is static: its table is copied here. */
static const esp_stub_t *stub_for(esp_loader_t *loader, target_chip_t chip, void *ctx)
{
    (void)ctx;
    switch (chip) {
    case ESP8266_CHIP:  return &esp_stub_esp8266;
    case ESP32_CHIP:    return &esp_stub_esp32;
    case ESP32S2_CHIP:  return &esp_stub_esp32s2;
    case ESP32C3_CHIP:  return &esp_stub_esp32c3;
    case ESP32S3_CHIP:  return &esp_stub_esp32s3;
    case ESP32C2_CHIP:  return &esp_stub_esp32c2;
    case ESP32C5_CHIP:  return &esp_stub_esp32c5;
    case ESP32H2_CHIP:  return &esp_stub_esp32h2;
    case ESP32C6_CHIP:  return &esp_stub_esp32c6;
    case ESP32C61_CHIP: return &esp_stub_esp32c61;
    case ESP32S31_CHIP: return &esp_stub_esp32s31;
    case ESP32H21_CHIP: return &esp_stub_esp32h21;
    case ESP32H4_CHIP:  return &esp_stub_esp32h4;
    case ESP32P4_CHIP: {
        p4_power_on_flash(loader);
        esp_loader_target_security_info_t si;
        bool v3 = esp_loader_get_security_info(loader, &si) == ESP_LOADER_SUCCESS && si.eco_version >= 5;
        return v3 ? &esp_stub_esp32p4 : &esp_stub_esp32p4rev1;
    }
    default:            return NULL;
    }
}

static bool connect(ctx_t *c, bool stub)
{
    fl_phase(AOS_FLASHER_CONNECTING);
    aos_io_uart_set_baud(c->u, 115200);
    esp_loader_init_serial(&c->L, &c->P.port);
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    if (!c->P.lines) args.trials = 80;      /* about ten seconds to press the buttons */
    esp_loader_error_t e;
    if (stub) {
        e = esp_loader_connect_with_stub_provider(&c->L, &args, stub_for, NULL);
        if (e != ESP_LOADER_SUCCESS && !fl_cancel && esp_loader_get_target(&c->L) != ESP_UNKNOWN_CHIP) {
            /* it answered but the stub did not come up: the ROM can do it all, slower */
            fl_logf(L_WARN, _("El stub no arrancó (%s): sigo con la ROM"), err_text(e));
            esp_loader_init_serial(&c->L, &c->P.port);
            e = esp_loader_connect(&c->L, &args);
            stub = false;
        }
    } else {
        e = esp_loader_connect(&c->L, &args);
    }
    if (e == ESP_LOADER_SUCCESS && !stub && esp_loader_get_target(&c->L) == ESP32P4_CHIP)
        p4_power_on_flash(&c->L);
    if (fl_cancel) return false;
    if (e != ESP_LOADER_SUCCESS && e == ESP_LOADER_ERROR_TIMEOUT && !strncmp(J.port, "usb", 3))
        return fl_fail("%s", _("La placa no respondió: desenchufala y enchufala, o enchufala con BOOT apretado"));
    if (e != ESP_LOADER_SUCCESS)
        return fl_fail(e == ESP_LOADER_ERROR_TIMEOUT ? _("La placa no respondió: revisá TX/RX cruzados, GND, EN y BOOT")
                                                   : _("No se pudo conectar: %s"), err_text(e));
    fl_lock();
    fl_st.stub = stub;
    fl_st.baud = 115200;
    fl_unlock();
    return true;
}

static bool read_info(ctx_t *c)
{
    fl_phase(AOS_FLASHER_INFO);
    target_chip_t t = esp_loader_get_target(&c->L);
    const char *idf = t < ESP_MAX_CHIP ? CHIP_IDF[t] : NULL;
    uint8_t mac[6] = { 0 };
    bool mac_ok = t != ESP8266_CHIP && esp_loader_read_mac(&c->L, mac) == ESP_LOADER_SUCCESS;
    uint16_t rev = 0xFFFF;
    if (esp_loader_get_chip_revision(&c->L, &rev) != ESP_LOADER_SUCCESS) rev = 0xFFFF;
    bool secure = false, crypt = false;
    esp_loader_target_security_info_t si;
    if (t != ESP32_CHIP && t != ESP8266_CHIP && esp_loader_get_security_info(&c->L, &si) == ESP_LOADER_SUCCESS) {
        secure = si.secure_boot_enabled || si.secure_download_mode_enabled;
        sec_reply_t r;
        uint32_t got = 0;
        p_start_timer(&c->P.port, 100);
        if (loader_get_security_info_cmd(&c->L, &r, &got) == ESP_LOADER_SUCCESS && got >= 5)
            crypt = __builtin_popcount(r.flash_crypt_cnt) & 1;
    }
    uint32_t flash = 0;
    if (esp_loader_flash_detect_size(&c->L, &flash) != ESP_LOADER_SUCCESS) flash = 0;
    fl_lock();
    fl_st.chip_known = true;
    snprintf(fl_st.chip, sizeof fl_st.chip, "%s", idf ? aos_flasher_chip_label(idf) : "?");
    fl_st.revision = rev;
    memcpy(fl_st.mac, mac, 6);
    fl_st.mac_known = mac_ok;
    fl_st.flash_size = flash;
    fl_st.secure = secure || crypt;
    fl_unlock();
    char revs[12] = "";
    if (rev != 0xFFFF) snprintf(revs, sizeof revs, " v%u.%u", rev / 100, rev % 100);
    char macs[24] = "";
    if (mac_ok) snprintf(macs, sizeof macs, ", MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char fl[16] = "?";
    if (flash) snprintf(fl, sizeof fl, "%u MB", (unsigned)(flash >> 20));
    fl_logf(L_OK, "%s%s%s, flash %s%s", fl_st.chip, revs, macs, fl, fl_st.stub ? ", stub" : ", ROM");
    if (secure) fl_logf(L_WARN, "%s", _("Secure boot o descarga segura activos"));
    if (crypt && J.job == AOS_FLASHER_JOB_FLASH)
        return fl_fail("%s", _("La flash está cifrada: grabarla en claro la deja sin arrancar"));
    if (crypt && J.job == AOS_FLASHER_JOB_READ)
        fl_logf(L_WARN, "%s", _("La flash está cifrada: el volcado sale cifrado, sirve para esta misma placa"));
    if (J.job == AOS_FLASHER_JOB_FLASH && J.src.chip[0] && idf && strcasecmp(J.src.chip, idf))
        return fl_fail(_("El firmware es para %s y la placa es %s"), aos_flasher_chip_label(J.src.chip), fl_st.chip);
    /* the bootloader refuses to start outside its range: say it now, not
     * after a flash that leaves the board without a program */
    if (J.job == AOS_FLASHER_JOB_FLASH && J.src.rev_max && rev != 0xFFFF &&
        (rev < J.src.rev_min || rev > J.src.rev_max))
        return fl_fail(_("El firmware acepta la revisión v%u.%u a v%u.%u y el chip es v%u.%u: no arrancaría"),
                     J.src.rev_min / 100, J.src.rev_min % 100, J.src.rev_max / 100, J.src.rev_max % 100,
                     rev / 100, rev % 100);
    return true;
}

/* The fast speed, checked with a read; if it does not answer, back to 115200
 * through a fresh reset. */
static bool go_fast(ctx_t *c)
{
    uint32_t baud = J.opts.baud ? J.opts.baud : BAUD_FAST;
    target_chip_t t = esp_loader_get_target(&c->L);
    if (baud <= 115200 || t == ESP8266_CHIP) return true;
    fl_phase(AOS_FLASHER_BAUD);
    esp_loader_error_t e = esp_loader_change_transmission_rate(&c->L, baud);
    uint8_t mac[6];
    if (e == ESP_LOADER_SUCCESS) e = esp_loader_read_mac(&c->L, mac);
    if (fl_cancel) return false;
    if (e == ESP_LOADER_SUCCESS) {
        fl_logf(L_INFO, _("Velocidad: %u baudios"), (unsigned)baud);
        fl_lock();
        fl_st.baud = baud;
        fl_unlock();
        return true;
    }
    fl_logf(L_WARN, _("A %u baudios no contesta: vuelvo a 115200"), (unsigned)baud);
    if (!c->P.lines) return fl_fail(_("La placa quedó a %u baudios: reseteala y probá de nuevo"), (unsigned)baud);
    return connect(c, fl_st.stub);
}

static bool write_file(ctx_t *c, int i)
{
    const aos_flasher_file_t *f = &J.src.files[i];
    char path[330];
    if (J.src.project) snprintf(path, sizeof path, "%s/%s", J.src.path, f->name);
    else snprintf(path, sizeof path, "%s", J.src.path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return fl_fail(_("No se pudo abrir %s"), f->name);

    target_chip_t t = esp_loader_get_target(&c->L);
    bool stub = fl_st.stub;
    uint32_t block = !stub ? 1024 : fl_st.baud >= 460800 ? BLOCK_MAX : 4096;
    uint32_t padded = (f->size + 3) & ~3u;
    esp_loader_flash_cfg_t cfg = {
        .offset = f->offset, .image_size = padded, .block_size = block,
        /* the ESP8266's ROM has no MD5; its stub has */
        .skip_verify = J.opts.no_verify || (t == ESP8266_CHIP && !stub),
    };
    fl_lock();
    fl_st.file_index = i;
    snprintf(fl_st.file_name, sizeof fl_st.file_name, "%s", f->name);
    fl_st.file_offset = f->offset;
    fl_st.phase = AOS_FLASHER_ERASING;
    fl_unlock();
    fl_logf(L_INFO, "0x%05x %s: %u KB", (unsigned)f->offset, f->name, (unsigned)((f->size + 1023) / 1024));
    esp_loader_error_t e = esp_loader_flash_start(&c->L, &cfg);
    if (e != ESP_LOADER_SUCCESS) { fclose(fp); return fl_cancel ? false : fl_fail(_("0x%x: no empezó (%s)"), (unsigned)f->offset, err_text(e)); }

    fl_phase(AOS_FLASHER_WRITING);
    uint32_t left = padded, file_left = f->size;
    while (left && !fl_cancel) {
        uint32_t want = left < block ? left : block;
        size_t n = fread(c->buf, 1, want, fp);
        if (n < want) memset(c->buf + n, 0xFF, want - n);      /* the padding to four */
        if (n == 0 && want > 3) { fclose(fp); return fl_fail(_("%s: no se pudo leer de la tarjeta"), f->name); }
        uint64_t t0 = aos_hal_uptime_ms();
        e = esp_loader_flash_write(&c->L, &cfg, c->buf, want);
        J.io_ms += aos_hal_uptime_ms() - t0;
        J.io_bytes += want;
        if (e != ESP_LOADER_SUCCESS) { fclose(fp); return fl_cancel ? false : fl_fail(_("0x%x: falló la escritura (%s)"), (unsigned)(f->offset + padded - left), err_text(e)); }
        left -= want;
        uint32_t add = want < file_left ? want : file_left;     /* the padding is not the file's */
        file_left -= add;
        fl_progress(add);
    }
    fclose(fp);
    if (fl_cancel) return false;
    if (!cfg.skip_verify) fl_phase(AOS_FLASHER_VERIFYING);
    e = esp_loader_flash_finish(&c->L, &cfg);
    if (e != ESP_LOADER_SUCCESS)
        return fl_cancel ? false : fl_fail("0x%x %s: %s", (unsigned)f->offset, f->name, err_text(e));
    if (cfg.skip_verify) {
        fl_lock();
        fl_st.verified = false;
        fl_unlock();
    }
    fl_logf(L_OK, "0x%05x %s%s", (unsigned)f->offset, f->name, cfg.skip_verify ? _(": escrito") : _(": escrito, MD5 bien"));
    return true;
}

/* A dump: the range asked for (all of the flash by default) in pieces, to
 * the core's file. */
static bool read_flash(ctx_t *c)
{
    uint32_t size = fl_st.flash_size, from = J.opts.read_from, len = J.opts.read_len;
    if (size) {
        if (from >= size)
            return fl_fail(_("0x%X queda fuera de la flash (%u KB)"), (unsigned)from, (unsigned)(size >> 10));
        if (!len || len > size - from) len = size - from;
    } else if (!len) {
        return fl_fail("%s", _("No se sabe el tamaño de la flash: indicá cuánto leer"));
    }
    if (!fl_dump_open(from, len)) return false;
    fl_phase(AOS_FLASHER_READING);
    bool verified = fl_st.stub;
    uint32_t done = 0;
    while (done < len && !fl_cancel) {
        uint32_t n = len - done < BLOCK_MAX ? len - done : BLOCK_MAX;
        uint64_t t0 = aos_hal_uptime_ms();
        esp_loader_error_t e = esp_loader_flash_read(&c->L, c->buf, from + done, n);
        J.io_ms += aos_hal_uptime_ms() - t0;
        J.io_bytes += n;
        if (e != ESP_LOADER_SUCCESS) {
            fl_dump_close(false, false, NULL);
            return fl_cancel ? false : fl_fail(_("0x%X: falló la lectura (%s)"), (unsigned)(from + done), err_text(e));
        }
        if (!fl_dump_write(c->buf, n)) { fl_dump_close(false, false, NULL); return false; }
        done += n;
    }
    if (fl_cancel) { fl_dump_close(false, false, NULL); return false; }
    char extra[96];
    snprintf(extra, sizeof extra, "\"family\":\"esp\",\"stub\":%s,\"baud\":%u", fl_st.stub ? "true" : "false",
             (unsigned)fl_st.baud);
    fl_dump_close(true, verified, extra);
    if (!verified) fl_logf(L_WARN, "%s", _("Leído con la ROM: sin verificar"));
    return true;
}

static bool erase_flash(ctx_t *c)
{
    fl_phase(AOS_FLASHER_ERASING);
    if (fl_st.flash_size) fl_logf(L_INFO, _("Borrando toda la flash (%u MB)"), (unsigned)(fl_st.flash_size >> 20));
    uint64_t t0 = aos_hal_uptime_ms();
    esp_loader_error_t e = esp_loader_flash_erase(&c->L);
    if (fl_cancel) return false;
    if (e != ESP_LOADER_SUCCESS) return fl_fail(_("No se pudo borrar: %s"), err_text(e));
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - t0);
    fl_logf(L_OK, _("Flash borrada en %u,%u s"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    return true;
}

static bool run(ctx_t *c)
{
    fl_phase(AOS_FLASHER_OPENING);
    if (J.job == AOS_FLASHER_JOB_FLASH && J.src.kind == AOS_FLASHER_IMAGE)
        return fl_fail(_("%s no es una imagen de ESP: es para un Arduino u otro micro"), J.src.name);
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    bool usb = !strncmp(J.port, "usb", 3);      /* a USB serial port of the host */
    if (!usb && (!pt || pt->kind != AOS_PORT_UART)) return fl_fail(_("No hay un puerto serie %s"), J.port);
    c->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
    /* A USB device can be away for a moment: a chip on its own USB that
     * restarts (an ESP32-C3 in a boot loop was gone 0.4 s out of every
     * 2.7, 2026-10-05) comes back as a new device. Up to 3 s for it. */
    for (int i = 0; !c->u && usb && i < 30 && !fl_cancel; i++) {
        aos_hal_sleep_ms(100);
        c->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
    }
    if (!c->u && usb) return fl_fail(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
    if (!c->u) {
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        return o ? fl_fail(_("%s está ocupado: lo tiene %s"), J.port, o) : fl_fail(_("No se pudo abrir %s"), J.port);
    }
    c->P.port.ops = &PORT_OPS;
    c->P.u = c->u;
    c->P.lines = aos_io_uart_lines(c->u, -1, -1);
    c->P.jtag = aos_io_uart_is_usb_jtag(c->u);
    fl_lock();
    fl_st.lines = c->P.lines;
    fl_unlock();
    int en = -1, boot = -1;
    aos_io_port_lines(J.port, &en, &boot);
    if (c->P.jtag) fl_logf(L_INFO, _("%s: la USB-Serial-JTAG del chip, se reinicia sola"), aos_io_uart_desc(c->u));
    else if (usb) fl_logf(L_INFO, _("%s, EN por RTS y BOOT por DTR"), aos_io_uart_desc(c->u));
    else if (c->P.lines) fl_logf(L_INFO, _("%s, EN GPIO%d, BOOT GPIO%d"), aos_io_uart_desc(c->u), en, boot);
    else if (en >= 0 || boot >= 0) fl_logf(L_WARN, _("%s: EN/BOOT ocupados por %s"), aos_io_uart_desc(c->u), aos_io_owner(en >= 0 ? en : boot) ? aos_io_owner(en >= 0 ? en : boot) : _("otro"));
    else fl_logf(L_WARN, _("%s sin EN/BOOT: poné la placa en modo descarga (BOOT apretado y reset)"), aos_io_uart_desc(c->u));

    /* the stub for anything but a look: it reads in 4 KB packets with an
     * MD5 and erases the whole chip in one command */
    bool stub = J.job != AOS_FLASHER_JOB_DETECT && !J.opts.no_stub;
    bool ok = connect(c, stub);
    if (!ok && usb && !fl_cancel) {
        /* once more on a port opened again: a USB port whose input had
         * died (a failed transfer, which the driver never resubmits) comes
         * back opened anew from the HAL (aos_usb_serial_p4.c) */
        fl_logf(L_WARN, "%s", _("No respondió: reabro el puerto y pruebo otra vez"));
        aos_io_uart_close(c->u);
        c->u = NULL;
        for (int i = 0; !c->u && i < 30 && !fl_cancel; i++) {
            c->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
            if (!c->u) aos_hal_sleep_ms(100);
        }
        if (!c->u) return fl_fail(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
        c->P.u = c->u;
        ok = connect(c, stub);
    }
    if (!ok) return false;
    if (!read_info(c)) return false;
    if (J.job == AOS_FLASHER_JOB_DETECT) return true;
    if (J.job == AOS_FLASHER_JOB_READ) return go_fast(c) && read_flash(c);
    if (J.job == AOS_FLASHER_JOB_ERASE) return erase_flash(c);

    uint32_t end = 0;
    for (int i = 0; i < J.src.nfiles; i++) {
        uint32_t e = J.src.files[i].offset + J.src.files[i].size;
        if (e > end) end = e;
    }
    if (fl_st.flash_size && end > fl_st.flash_size)
        return fl_fail(_("No entra: llega a %u KB y la flash tiene %u KB"), (unsigned)(end >> 10), (unsigned)(fl_st.flash_size >> 10));
    if (!go_fast(c)) return false;
    fl_lock();
    fl_st.verified = !J.opts.no_verify;
    fl_unlock();
    for (int i = 0; i < J.src.nfiles; i++) if (!write_file(c, i)) return false;
    return true;
}


bool fl_esp_run(void)
{
    ctx_t *c = calloc(1, sizeof *c);
    if (!c) return fl_fail("%s", _("Sin memoria"));
    c->buf = malloc(BLOCK_MAX);
    bool ok = c->buf ? run(c) : fl_fail("%s", _("Sin memoria"));
    if (c->u) {
        /* the target back to its own program, unless asked to stay */
        bool reset = !(J.job == AOS_FLASHER_JOB_FLASH && ok && J.opts.no_reset);
        if (reset && c->P.lines) {
            bool cancelled = fl_cancel;
            fl_phase(AOS_FLASHER_RESETTING);
            fl_cancel = false;      /* a cancelled job still resets */
            esp_loader_reset_target(&c->L);
            fl_cancel = cancelled;
            if (J.job == AOS_FLASHER_JOB_FLASH && ok) fl_logf(L_INFO, "%s", _("Placa reiniciada"));
        } else if (ok && J.job == AOS_FLASHER_JOB_FLASH && !c->P.lines) {
            fl_logf(L_WARN, "%s", _("Reseteá la placa para que arranque el firmware nuevo"));
        }
        aos_io_uart_close(c->u);
    }
    free(c->buf);
    free(c);
    return ok;
}

#else   /* no esp-serial-flasher: the simulator before any firmware build */

bool fl_esp_run(void)
{
    return fl_fail("%s", _("Falta esp-serial-flasher: compilá el firmware una vez (managed_components)"));
}

#endif
