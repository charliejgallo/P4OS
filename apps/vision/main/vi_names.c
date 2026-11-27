/*
 * P4OS - Vision: what each thing found is called, and its colour. The COCO
 * classes in the dataset's order (YOLO11n's class index), the gestures in
 * the hand model's (main/ve_api.h).
 */
#include "vision.h"

#include "aos_i18n.h"
#include "aos_theme.h"

#include <stdio.h>

static const char *const COCO[80] = {
    N_("persona"),         N_("bicicleta"),      N_("auto"),            N_("moto"),
    N_("avión"),           N_("colectivo"),      N_("tren"),            N_("camión"),
    N_("barco"),           N_("semáforo"),       N_("hidrante"),        N_("cartel de pare"),
    N_("parquímetro"),     N_("banco"),          N_("pájaro"),          N_("gato"),
    N_("perro"),           N_("caballo"),        N_("oveja"),           N_("vaca"),
    N_("elefante"),        N_("oso"),            N_("cebra"),           N_("jirafa"),
    N_("mochila"),         N_("paraguas"),       N_("cartera"),         N_("corbata"),
    N_("valija"),          N_("frisbee"),        N_("esquís"),          N_("tabla de snow"),
    N_("pelota"),          N_("barrilete"),      N_("bate"),            N_("guante de béisbol"),
    N_("patineta"),        N_("tabla de surf"),  N_("raqueta"),         N_("botella"),
    N_("copa de vino"),    N_("taza"),           N_("tenedor"),         N_("cuchillo"),
    N_("cuchara"),         N_("bol"),            N_("banana"),          N_("manzana"),
    N_("sándwich"),        N_("naranja"),        N_("brócoli"),         N_("zanahoria"),
    N_("pancho"),          N_("pizza"),          N_("dona"),            N_("torta"),
    N_("silla"),           N_("sillón"),         N_("planta"),          N_("cama"),
    N_("mesa"),            N_("inodoro"),        N_("televisor"),       N_("notebook"),
    N_("mouse"),           N_("control remoto"), N_("teclado"),         N_("celular"),
    N_("microondas"),      N_("horno"),          N_("tostadora"),       N_("pileta de cocina"),
    N_("heladera"),        N_("libro"),          N_("reloj"),           N_("florero"),
    N_("tijera"),          N_("osito de peluche"), N_("secador de pelo"), N_("cepillo de dientes"),
};

static const char *const GESTURE[VE_G_COUNT] = {
    N_("uno"),   N_("dos"), N_("tres"), N_("cuatro"),     N_("cinco"),      N_("pulgar arriba"),
    N_("ok"),    N_("mano"), N_("llamame"), N_("pulgar abajo"), N_("sin mano"),
};

const char *vi_coco_name(int cls)
{
    return (cls >= 0 && cls < 80) ? _(COCO[cls]) : "?";
}

const char *vi_gesture_name(int g)
{
    /* the detector saw a hand: "no gesture" and "no hand" are just a hand */
    if (g == VE_G_NO_HAND) g = VE_G_NONE;
    return (g >= 0 && g < VE_G_COUNT) ? _(GESTURE[g]) : "?";
}

const char *vi_mode_name(vi_mode_t m)
{
    switch (m) {
    case VI_M_FACE: return _("Caras");
    case VI_M_PERSON: return _("Personas");
    case VI_M_COCO: return _("Objetos");
    case VI_M_HAND: return _("Manos");
    case VI_M_CODES: return _("Códigos");
    default: return "";
    }
}

const char *vi_mode_noun(vi_mode_t m, int n)
{
    switch (m) {
    case VI_M_FACE: return n == 1 ? _("cara") : _("caras");
    case VI_M_PERSON: return n == 1 ? _("persona") : _("personas");
    case VI_M_COCO: return n == 1 ? _("objeto") : _("objetos");
    case VI_M_HAND: return n == 1 ? _("mano") : _("manos");
    case VI_M_CODES: return n == 1 ? _("código") : _("códigos");
    default: return "";
    }
}

/* one colour per class, steady from frame to frame */
lv_color_t vi_class_color(vi_mode_t m, int cls)
{
    static const uint32_t PAL[] = {0x30D158, 0x0A84FF, 0xFF9F0A, 0xBF5AF2, 0xFF375F, 0x40C8E0, 0xFFD60A, 0x64D2FF};
    switch (m) {
    case VI_M_FACE: return lv_color_hex(0x30D158);
    case VI_M_PERSON: return lv_color_hex(0x0A84FF);
    case VI_M_HAND: return lv_color_hex(0xFF9F0A);
    case VI_M_CODES: return lv_color_hex(0xFFD60A);
    default: break;
    }
    if (cls < 0) cls = 0;
    return lv_color_hex(PAL[cls % (int)(sizeof PAL / sizeof PAL[0])]);
}
