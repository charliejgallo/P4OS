/*
 * Claudito - object sprites (see cl_sprites.h)
 *
 * Every sprite has an outline or a shaded side in a darker step of its colour
 * and a highlight in a lighter one, with the light from the top left, as the
 * window in the living room has it. The sizes are the watch's, so they sit
 * where they always sat in the tray, the bar and the scenes.
 */
#include "cl_sprites.h"

/* --- food ----------------------------------------------------------------- */

const char *const cl_spr_apple[11] = {
    "....NN.....",
    "....N.ggG..",
    "..rrrNgG...",
    ".rqwrrrrrR.",
    "rqwwrrrrrrR",
    "rqwrrrrrrrR",
    "rrrrrrrrrrR",
    "rrrrrrrrrRR",
    ".rrrrrrrRR.",
    "..RRRRRRR..",
    "...QQ.QQ...",
};

const char *const cl_spr_pizza[11] = {
    "NnnnnnnnnnN",
    "Njjjjjjjjjn",
    ".yyfyyyyyY.",
    ".yrrygyyyY.",
    "..rRyyrrY..",
    "..yyyyrRY..",
    "...ygyyY...",
    "...yrryY...",
    "....rRY....",
    "....yyY....",
    ".....Y.....",
};

const char *const cl_spr_cookie[11] = {
    "...nnnnn...",
    "..njjjjnn..",
    ".njjKjjnnN.",
    "njjjjjnKnnN",
    "njKjnnnnnnN",
    "nnnnnnKnnnN",
    "nnnKnnnnnNN",
    "nnnnnnnKnNN",
    ".nnKnnnnNN.",
    "..NnnnNNN..",
    "...NNNNN...",
};

const char *const cl_spr_cake[11] = {
    "....q......",
    "...rrR.....",
    "..aamaa....",
    ".aammmmm...",
    "ammmmmmmM..",
    "mMmmMmmMm..",
    "sMssMssMs..",
    "sssssssss..",
    "SmmmmmmmS..",
    "sssssssss..",
    ".SSSSSSS...",
};

/* --- toys ----------------------------------------------------------------- */

const char *const cl_spr_ball[11] = {
    "...rrrrr...",
    "..rqqrrrr..",
    ".rqwwrrrrR.",
    "rqwrrrrrrrR",
    "rrrrrrrrrrR",
    "wwwwwwwwwWW",
    "rrrrrrrrrRR",
    "rrrrrrrrrRR",
    ".rrrrrrrRR.",
    "..RRRRRRR..",
    "...QQQQQ...",
};

const char *const cl_spr_balloon[11] = {
    "...ppp.....",
    "..pvvpp....",
    ".pvwpppP...",
    ".pvppppP...",
    ".ppppppP...",
    ".ppppppP...",
    ".pppppPP...",
    "..ppppP....",
    "..PpppP....",
    "...PPP.....",
    "....P......",
};

const char *const cl_spr_dice[9] = {
    ".lllllll.",
    "lkwwwwwkW",
    "lwwwwwwwW",
    "lwwwkwwwW",
    "lwwwwwwwW",
    "lkwwwwwkW",
    "WwwwwwwwW",
    "WWWWWWWWW",
    ".DDDDDDD.",
};

/* the bubbles are rings: what shows through them is whatever is behind */
const char *const cl_spr_bubbles[11] = {
    "...ccc.....",
    "..cw..c....",
    "..c...C....",
    "...CCC.....",
    ".......cc..",
    "......cw.c.",
    "......c..C.",
    ".......CC..",
    "..cc.......",
    ".cw.C......",
    "..CC.......",
};

/* --- tools ---------------------------------------------------------------- */

/* a kitchen sponge: the yellow body and the green scouring side */
const char *const cl_spr_sponge[11] = {
    ".........c.",
    "..c.....cwc",
    ".cwc.....c.",
    "..c........",
    "fffffffffff",
    "fyyYyyyyYyy",
    "yyyyyyYyyyY",
    "yYyyyyyyyyY",
    "yyyyYyyyYyY",
    "ggggggggggg",
    "GGGGGGGGGGG",
};

const char *const cl_spr_hand[9] = {
    ".s.s.s...",
    "ss.s.s.s.",
    "sSsSsSsss",
    "sssssssSS",
    "sssssssS.",
    ".sssSssS.",
    "..sssssS.",
    "...sssSS.",
    "....SSS..",
};

const char *const cl_spr_moon[9] = {
    "...fyy...",
    "..fyyyy..",
    ".fyyY....",
    ".yyY.....",
    "fyy......",
    ".yyY.....",
    ".yyyY....",
    "..YyyyY..",
    "...YYY...",
};

const char *const cl_spr_door[11] = {
    ".NNNNNNNNN.",
    ".NjnnnnnnN.",
    ".NncccccnN.",
    ".NncuccCnN.",
    ".NnnnnnnnN.",
    ".NnNNnNNyN.",
    ".NnNjnNjnN.",
    ".NnNNnNNnN.",
    ".NnnnnnnnN.",
    ".NNNNNNNNN.",
    "...........",
};

const char *const cl_spr_tree_icon[11] = {
    "...iig.....",
    "..iiggg....",
    ".iigggGg...",
    "ggggrggGG..",
    ".gggggGGG..",
    "..GgGGGG...",
    "...jnN.....",
    "...jnN.....",
    "...jnN.....",
    "..GGGGG....",
    "...........",
};

/* --- scene ornaments ------------------------------------------------------ */

const char *const cl_spr_plant[16] = {
    "......i......",
    ".....igG.....",
    "..i..ggG..i..",
    ".igG.gGG.igG.",
    "..ggg.g.ggG..",
    "...gG.g.gG...",
    ".i...gGG...i.",
    "igG..gGG..igG",
    ".ggG.gGG.gGG.",
    "...G.gGG.G...",
    "......G......",
    ".xxxxxxxxxxX.",
    ".XXXXXXXXXXX.",
    "..xxxxxxxxX..",
    "..xxxxxxxXX..",
    "...XXXXXXX...",
};

const char *const cl_spr_flower[6] = {
    ".a.m..",
    "amymM.",
    ".mym..",
    "..g...",
    ".gG...",
    "..g...",
};

const char *const cl_spr_butterfly[7] = {
    "vp.....pv",
    "pPp.k.pPp",
    "pvppkppvp",
    "..ppkpp..",
    "pppPkPppp",
    "pPp.k.pPp",
    "pp.....pp",
};

/* a blue bowl with a heap of kibble */
const char *const cl_spr_bowl[7] = {
    "...nNnnNn....",
    "..nNnjnNnnN..",
    ".lwwwwwwwwwW.",
    "bbbbbbbbbbbbB",
    ".bBbbbbbbbbB.",
    "..BBBBBBBBB..",
    "....BBBBB....",
};

const char *const cl_spr_lamp[13] = {
    "...yyyyy...",
    "..fyyyyyY..",
    ".fyyyyyyyY.",
    "fyyyyyyyyyY",
    "YYYYYYYYYYY",
    "....ff.....",
    ".....D.....",
    ".....D.....",
    ".....D.....",
    ".....D.....",
    "....DDd....",
    "...DDDDd...",
    "..ddddddd..",
};

const char *const cl_spr_bone[5] = {
    "lw.....wl",
    "wwwwwwwwW",
    ".wwwwwwW.",
    "wWWWWWWWW",
    "WW.....WW",
};
