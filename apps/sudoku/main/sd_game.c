/*
 * SUDOKU - a game: what a digit does on the board, the notes, the undo
 * stack, the hints and what the end pays.
 *
 * Undo is deep (3000 changes, saved with the game) and works by cells: an
 * action is one or more entries chained with 'more' (a digit placed plus
 * the notes it cleared around it is one action, undone in one go).
 *
 * A hint looks at the board as it is: first a digit that is wrong; else the
 * logical solver's next step from the values on the board (not from the
 * player's notes, which may be wrong or missing), minus what earlier hints
 * already ruled out, so the next hint moves on instead of repeating itself.
 *
 * The coins: a base by mode and level, then half again without hints, a
 * quarter without mistakes, a quarter under the level's par time, half
 * without help (errors not shown, no automatic notes), and two a day of
 * streak up to twenty. The daily challenge pays double its base.
 */
#include "sd_app.h"

#include <stdio.h>
#include <string.h>

sd_game_t  sd_G;
sd_shape_t sd_shape;

const char *const SD_MODE_NAME[SD_MODES] = { N_("Clásico"), N_("6×6"), N_("Killer"), N_("Diagonal"),
                                             N_("Desafío del día") };
const char *const SD_LEVEL_NAME[SD_LEVELS] = { N_("Fácil"), N_("Medio"), N_("Difícil"), N_("Experto"),
                                               N_("Maestro") };

static const uint16_t BASE[SD_MODES][SD_LEVELS] = {
    { 15, 25, 40, 60, 90 }, { 8, 12 }, { 30, 45, 70 }, { 20, 30, 45, 65 }, { 0 },
};
/* par time, in minutes */
static const uint8_t PAR[SD_MODES][SD_LEVELS] = {
    { 6, 10, 15, 22, 30 }, { 3, 5 }, { 12, 20, 30 }, { 7, 11, 16, 24 }, { 0 },
};

/* ---- the undo stack ----------------------------------------------------------------- */

static void push(int c, int oldv, int newv, uint16_t oldn, uint16_t newn, bool more)
{
    if (sd_G.nundo >= SD_UNDO_MAX) {
        /* full: the oldest action goes */
        int k = 0;
        while (k < sd_G.nundo && sd_G.undo[k].more) k++;
        k++;
        memmove(sd_G.undo, sd_G.undo + k, (size_t)(sd_G.nundo - k) * sizeof(sd_undo_t));
        sd_G.nundo = (uint16_t)(sd_G.nundo - k);
    }
    sd_undo_t *u = &sd_G.undo[sd_G.nundo++];
    u->cell = (uint8_t)c;
    u->oldv = (uint8_t)oldv;
    u->newv = (uint8_t)newv;
    u->oldn = oldn;
    u->newn = newn;
    u->more = more;
    sd_G.ntop = sd_G.nundo;
}

/* sets a cell and records it; 'more' chains it to the next one recorded */
static void change(int c, int v, uint16_t notes, bool more)
{
    if (sd_G.val[c] == v && sd_G.notes[c] == notes) return;
    push(c, sd_G.val[c], v, sd_G.notes[c], notes, more);
    sd_G.val[c] = (uint8_t)v;
    sd_G.notes[c] = notes;
    sd_board_cell(c);
}

/* the last entry of the action just recorded ends it */
static void close_action(void)
{
    if (sd_G.nundo) sd_G.undo[sd_G.nundo - 1].more = 0;
}

void sd_game_undo(void)
{
    if (!sd_G.nundo || sd_G.won) return;
    /* an action is the entries since the previous one with more == 0 */
    int end = sd_G.nundo - 1, start = end;
    while (start > 0 && sd_G.undo[start - 1].more) start--;
    for (int k = end; k >= start; k--) {
        sd_undo_t *u = &sd_G.undo[k];
        sd_G.val[u->cell] = u->oldv;
        sd_G.notes[u->cell] = u->oldn;
        sd_board_cell(u->cell);
    }
    sd_G.sel = (int8_t)sd_G.undo[start].cell;
    sd_G.nundo = (uint16_t)start;
    sd_board_all();
    sd_snd_play(SD_S_ERASE);
}

void sd_game_redo(void)
{
    if (sd_G.nundo >= sd_G.ntop || sd_G.won) return;
    int k = sd_G.nundo;
    int first = k;
    do {
        sd_undo_t *u = &sd_G.undo[k];
        sd_G.val[u->cell] = u->newv;
        sd_G.notes[u->cell] = u->newn;
        sd_board_cell(u->cell);
        k++;
    } while (k < sd_G.ntop && sd_G.undo[k - 1].more);
    sd_G.sel = (int8_t)sd_G.undo[first].cell;
    sd_G.nundo = (uint16_t)k;
    sd_board_all();
    sd_snd_play(SD_S_PLACE);
}

/* ---- the rules -------------------------------------------------------------------------- */

bool sd_game_wrong(int c)
{
    return sd_G.val[c] && sd_G.val[c] != sd_G.puz.sol[c];
}

int sd_game_remaining(int d)
{
    int k = sd_shape.n;
    for (int i = 0; i < sd_shape.ncells; i++)
        if (sd_G.val[i] == d && !sd_game_wrong(i)) k--;
    return k;
}

static bool unit_done(int u)
{
    for (int k = 0; k < sd_shape.unit_len[u]; k++) {
        int c = sd_shape.unit_cell[u][k];
        if (!sd_G.val[c] || sd_game_wrong(c)) return false;
    }
    return true;
}

static void check_end(void)
{
    for (int i = 0; i < sd_shape.ncells; i++)
        if (!sd_G.val[i]) return;
    for (int i = 0; i < sd_shape.ncells; i++) {
        if (sd_game_wrong(i)) {
            sd_ui_toast(_("El tablero está lleno, pero hay errores"));
            return;
        }
    }
    /* won */
    sd_G.won = true;
    uint32_t secs = sd_game_seconds();
    int m = sd_G.mode, l = sd_G.level;
    uint16_t base = m == SD_M_DAILY ? 0 : BASE[m][l];
    int par = m == SD_M_DAILY ? 0 : PAR[m][l];
    if (m == SD_M_DAILY) {
        /* the daily pays twice the base of what it is */
        int dm = sd_shape.ncages ? SD_M_KILLER : sd_shape.diag ? SD_M_DIAG : SD_M_CLASSIC;
        int dl = sd_G.puz.level < SD_MODE_LEVELS[dm] ? sd_G.puz.level : 0;
        base = (uint16_t)(BASE[dm][dl] * 2);
        par = PAR[dm][dl];
    }
    memset(sd_G.pay, 0, sizeof sd_G.pay);
    sd_G.pay[0] = base;
    if (!sd_G.hints) sd_G.pay[1] = (uint16_t)(base / 2);
    if (!sd_G.mistakes) sd_G.pay[2] = (uint16_t)(base / 4);
    if (par && secs <= (uint32_t)par * 60u) sd_G.pay[3] = (uint16_t)(base / 4);
    if (!sd_G.assisted && !sd_D.mark_errors) sd_G.pay[4] = (uint16_t)(base / 2);

    /* the streak: days in a row with a game won */
    uint32_t ymd;
    if (sd_today(&ymd, NULL)) {
        uint32_t day = sd_ymd_days(ymd);
        if (sd_D.last_day + 1 == day) sd_D.streak++;
        else if (sd_D.last_day != day) sd_D.streak = 1;
        sd_D.last_day = day;
        if (sd_D.streak > sd_D.best_streak) sd_D.best_streak = sd_D.streak;
        if (m == SD_M_DAILY && sd_G.daily == ymd && sd_D.daily_done != ymd) {
            uint32_t prev = sd_D.daily_done ? sd_ymd_days(sd_D.daily_done) : 0;
            sd_D.daily_streak = prev + 1 == day ? (uint16_t)(sd_D.daily_streak + 1) : 1;
            sd_D.daily_done = ymd;
            sd_D.daily_time_s = secs;
        }
    }
    int st = sd_D.streak > 10 ? 10 : sd_D.streak;
    sd_G.pay[5] = (uint16_t)(st > 1 ? st * 2 : 0);
    uint32_t total = 0;
    for (int i = 0; i < 6; i++) total += sd_G.pay[i];
    sd_G.coins_won = (uint16_t)total;
    sd_D.coins += total;
    sd_D.coins_total += total;

    sd_stat_t *s = &sd_D.st[m][m == SD_M_DAILY ? 0 : l];
    s->won++;
    s->sum_s += secs;
    sd_G.record = !s->best_s || secs < s->best_s;
    if (sd_G.record) s->best_s = secs;
    sd_G.active = false;
    sd_data_save();
    sd_game_forget();
    sd_G.sel = -1;
    sd_board_all();
    sd_snd_play(SD_S_WIN);
    sd_ui_won();
}

/* the digit leaves the notes of every cell that sees c */
static void clean_peers(int c, int d)
{
    for (int k = 0; k < sd_shape.npeers[c]; k++) {
        int p = sd_shape.peer[c][k];
        if (sd_G.notes[p] & (1u << d)) change(p, sd_G.val[p], (uint16_t)(sd_G.notes[p] & ~(1u << d)), true);
    }
}

void sd_game_input(int d)
{
    if (!sd_G.active || sd_G.won) return;
    if (d >= 1 && d <= sd_shape.n) sd_G.digit = (uint8_t)d;
    int c = sd_G.sel;
    if (c < 0 || c >= sd_shape.ncells || sd_G.puz.given[c]) {
        sd_board_all();
        return;
    }
    if (d == 0) {
        sd_game_erase();
        return;
    }
    if (sd_G.notes_mode) {
        if (sd_G.val[c]) return;
        change(c, 0, (uint16_t)(sd_G.notes[c] ^ (1u << d)), false);
        sd_board_all();
        sd_snd_play(SD_S_NOTE);
        return;
    }
    if (sd_G.val[c] == d) {
        /* the same digit again takes it away */
        change(c, 0, 0, false);
        sd_board_all();
        sd_snd_play(SD_S_ERASE);
        return;
    }
    bool wrong = d != sd_G.puz.sol[c];
    change(c, d, 0, true);
    if (sd_D.auto_clean && !wrong) clean_peers(c, d);
    close_action();
    if (wrong) {
        sd_G.mistakes++;
        if (sd_D.mark_errors) sd_G.assisted = true;
        sd_snd_play(sd_D.mark_errors ? SD_S_ERROR : SD_S_PLACE);
    } else {
        /* a unit finished chimes */
        bool unit = false;
        for (int k = 0; k < 4; k++) {
            int u = sd_shape.cell_unit[c][k];
            if (u != 0xFF && unit_done(u)) unit = true;
        }
        sd_snd_play(unit ? SD_S_UNIT : SD_S_PLACE);
    }
    sd_board_all();
    check_end();
}

void sd_game_erase(void)
{
    int c = sd_G.sel;
    if (!sd_G.active || sd_G.won || c < 0 || c >= sd_shape.ncells || sd_G.puz.given[c]) return;
    if (!sd_G.val[c] && !sd_G.notes[c]) return;
    change(c, 0, 0, false);
    sd_board_all();
    sd_snd_play(SD_S_ERASE);
}

/* Every empty cell gets the candidates its row, column, box (and diagonal,
 * and cage) still allow: counts as help. */
void sd_game_autonotes(void)
{
    if (!sd_G.active || sd_G.won) return;
    sd_logic_t L;
    sd_logic_init(&L, &sd_shape, sd_G.val);
    bool any = false;
    for (int c = 0; c < sd_shape.ncells; c++) {
        if (sd_G.val[c]) continue;
        uint16_t m = (uint16_t)(L.cand[c] & ~sd_G.hint_elim[c]);
        if (m != sd_G.notes[c]) {
            change(c, 0, m, true);
            any = true;
        }
    }
    if (any) {
        close_action();
        sd_G.assisted = true;
    }
    sd_board_all();
    sd_snd_play(SD_S_NOTE);
}

bool sd_game_hint(sd_step_t *st)
{
    if (!sd_G.active || sd_G.won) return false;
    memset(st, 0, sizeof *st);
    st->cell = -1;
    st->unit = st->unit2 = 0xFF;
    /* a wrong digit first: nothing else makes sense on top of it */
    int from = sd_G.sel >= 0 ? sd_G.sel : 0;
    for (int k = 0; k < sd_shape.ncells; k++) {
        int c = (from + k) % sd_shape.ncells;
        if (sd_game_wrong(c) && !sd_G.puz.given[c]) {
            st->tech = SD_T_WRONG;
            st->cell = (int8_t)c;
            st->digit = sd_G.val[c];
            return true;
        }
    }
    sd_logic_t L;
    sd_logic_init(&L, &sd_shape, sd_G.val);
    for (int c = 0; c < sd_shape.ncells; c++) L.cand[c] &= (uint16_t)~sd_G.hint_elim[c];
    if (sd_logic_step(&L, SD_L_MASTER, st)) return true;
    /* nothing the solver can explain: the selected (or first empty) cell */
    int c = sd_G.sel >= 0 && !sd_G.val[sd_G.sel] ? sd_G.sel : -1;
    for (int k = 0; c < 0 && k < sd_shape.ncells; k++)
        if (!sd_G.val[k]) c = k;
    if (c < 0) return false;
    memset(st, 0, sizeof *st);
    st->tech = SD_T_REVEAL;
    st->cell = (int8_t)c;
    st->digit = sd_G.puz.sol[c];
    st->unit = st->unit2 = 0xFF;
    return true;
}

/* "Aplicar": the step done on the board, as the player would */
void sd_game_hint_apply(const sd_step_t *st)
{
    if (!sd_G.active || sd_G.won) return;
    if (st->tech == SD_T_WRONG) {
        sd_G.sel = st->cell;
        change(st->cell, 0, 0, false);
        sd_board_all();
        sd_snd_play(SD_S_ERASE);
        return;
    }
    if (st->cell >= 0 && st->digit) {
        bool notes = sd_G.notes_mode;
        sd_G.notes_mode = false;
        sd_G.sel = st->cell;
        sd_game_input(st->digit);
        sd_G.notes_mode = notes;
        return;
    }
    for (int k = 0; k < st->nelim; k++) {
        int c = st->elim_cell[k], d = st->elim_digit[k];
        sd_G.hint_elim[c] |= (uint16_t)(1u << d);
        if (sd_G.notes[c] & (1u << d)) change(c, sd_G.val[c], (uint16_t)(sd_G.notes[c] & ~(1u << d)), true);
    }
    close_action();
    sd_board_all();
    sd_snd_play(SD_S_NOTE);
}

void sd_game_new(int mode, int level, uint32_t daily, const sd_packed_t *p)
{
    memset(&sd_G, 0, sizeof sd_G);
    sd_G.active = true;
    sd_G.mode = (uint8_t)mode;
    sd_G.level = (uint8_t)level;
    sd_G.daily = daily;
    sd_G.puz = *p;
    sd_unpack_shape(p, &sd_shape);
    memcpy(sd_G.val, p->given, SD_MAXC);
    sd_G.sel = -1;
    sd_G.digit = 0;
    sd_D.st[mode][mode == SD_M_DAILY ? 0 : level].played++;
    sd_data_save();
    sd_game_save();
}

void sd_game_tick(uint32_t dt)
{
    if (sd_G.active && !sd_G.won) sd_G.elapsed_ms += dt;
}

uint32_t sd_game_seconds(void)
{
    return sd_G.elapsed_ms / 1000u;
}

/* ---- the words of a hint ------------------------------------------------------------------- */

const char *sd_tech_name(int t)
{
    switch (t) {
    case SD_T_FULL_HOUSE: return _("Última celda libre");
    case SD_T_NAKED_SINGLE: return _("Único candidato");
    case SD_T_HIDDEN_SINGLE: return _("Único lugar");
    case SD_T_CAGE_COMBO: return _("Combinaciones de la jaula");
    case SD_T_INNIE: return _("La regla del 45");
    case SD_T_POINTING: return _("Candidatos bloqueados");
    case SD_T_CLAIMING: return _("Candidatos bloqueados");
    case SD_T_NAKED_PAIR: return _("Par desnudo");
    case SD_T_HIDDEN_PAIR: return _("Par oculto");
    case SD_T_NAKED_TRIPLE: return _("Trío desnudo");
    case SD_T_HIDDEN_TRIPLE: return _("Trío oculto");
    case SD_T_XWING: return _("X-Wing");
    case SD_T_NAKED_QUAD: return _("Cuarteto desnudo");
    case SD_T_HIDDEN_QUAD: return _("Cuarteto oculto");
    case SD_T_SWORDFISH: return _("Pez espada");
    case SD_T_XYWING: return _("Ala XY");
    case SD_T_XYZWING: return _("Ala XYZ");
    case SD_T_CHAIN_SHORT: return _("Cadena corta");
    case SD_T_JELLYFISH: return _("Medusa");
    case SD_T_CHAIN_LONG: return _("Cadena larga");
    case SD_T_REVEAL: return _("Pista");
    case SD_T_WRONG: return _("Un error");
    default: return "";
    }
}

static const char *unit_noun(int u)
{
    if (u == 0xFF || u >= sd_shape.nunits) return _("fila");
    switch (sd_shape.unit_kind[u]) {
    case SD_U_ROW: return _("fila");
    case SD_U_COL: return _("columna");
    case SD_U_BOX: return _("caja");
    default: return _("diagonal");
    }
}

/* "3, 5 y 8" */
static void digits_list(uint16_t m, char *out, size_t n)
{
    int cnt = sd_popcount(m), k = 0;
    size_t o = 0;
    out[0] = 0;
    for (int d = 1; d <= 9; d++) {
        if (!(m & (1u << d))) continue;
        k++;
        const char *sep = k == 1 ? "" : k == cnt ? _(" y ") : ", ";
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%d", sep, d);
        if (o >= n) break;
    }
}

void sd_hint_text(const sd_step_t *st, char *out, size_t n)
{
    char ds[40];
    digits_list(st->digits, ds, sizeof ds);
    const char *u = unit_noun(st->unit);
    switch (st->tech) {
    case SD_T_WRONG:
        snprintf(out, n, _("El %d de la celda marcada no va ahí. Aplicar lo borra."), st->digit);
        break;
    case SD_T_REVEAL:
        snprintf(out, n, _("No hay un paso simple que explicar desde acá: la celda marcada es un %d."), st->digit);
        break;
    case SD_T_FULL_HOUSE:
        snprintf(out, n, _("Es la única celda vacía de su %s: le falta el %d."), u, st->digit);
        break;
    case SD_T_NAKED_SINGLE:
        snprintf(out, n, _("En esta celda sólo puede ir el %d: los demás números ya están en su fila, su columna "
                           "o su caja."),
                 st->digit);
        break;
    case SD_T_HIDDEN_SINGLE:
        snprintf(out, n, _("En esta %s el %d sólo puede ir acá."), u, st->digit);
        break;
    case SD_T_CAGE_COMBO:
        snprintf(out, n, _("Esta jaula suma %d: con lo que ya hay alrededor sólo le entran el %s. Lo demás sale."),
                 st->sum, ds);
        break;
    case SD_T_INNIE:
        if (st->cell >= 0)
            snprintf(out, n, _("Toda %s suma 45. Las jaulas que caen enteras adentro dejan %d para la celda "
                               "marcada: es un %d."),
                     u, st->sum, st->digit);
        else
            snprintf(out, n, _("Toda %s suma 45. Las jaulas que caen enteras adentro dejan %d para las celdas "
                               "marcadas: sólo les entran el %s."),
                     u, st->sum, ds);
        break;
    case SD_T_POINTING:
        snprintf(out, n, _("En esta caja el %s sólo puede ir sobre una %s: sale del resto de esa %s."), ds,
                 unit_noun(st->unit2), unit_noun(st->unit2));
        break;
    case SD_T_CLAIMING:
        snprintf(out, n, _("En esta %s el %s sólo puede ir dentro de una caja: sale del resto de esa caja."), u, ds);
        break;
    case SD_T_NAKED_PAIR:
    case SD_T_NAKED_TRIPLE:
    case SD_T_NAKED_QUAD:
        snprintf(out, n, _("Las celdas marcadas sólo pueden ser %s, entre todas: esos números salen del resto de "
                           "la %s."),
                 ds, u);
        break;
    case SD_T_HIDDEN_PAIR:
    case SD_T_HIDDEN_TRIPLE:
    case SD_T_HIDDEN_QUAD:
        snprintf(out, n, _("En esta %s el %s sólo pueden ir en las celdas marcadas: ahí sale todo lo demás."), u,
                 ds);
        break;
    case SD_T_XWING:
    case SD_T_SWORDFISH:
    case SD_T_JELLYFISH: {
        int k = st->tech == SD_T_XWING ? 2 : st->tech == SD_T_SWORDFISH ? 3 : 4;
        if (st->unit == SD_U_ROW)
            snprintf(out, n, _("En %d filas el %s sólo puede ir en las mismas %d columnas: sale del resto de esas "
                               "columnas."),
                     k, ds, k);
        else
            snprintf(out, n, _("En %d columnas el %s sólo puede ir en las mismas %d filas: sale del resto de esas "
                               "filas."),
                     k, ds, k);
        break;
    }
    case SD_T_XYWING:
        snprintf(out, n, _("La celda del medio tiene dos opciones y cada una obliga a una punta a ser %d. Una de las "
                           "dos puntas lo es: el %d sale de las celdas que ven a las dos."),
                 st->digit, st->digit);
        break;
    case SD_T_XYZWING:
        snprintf(out, n, _("Entre la celda del medio y sus dos puntas, alguna es %d: el %d sale de las celdas que "
                           "ven a las tres."),
                 st->digit, st->digit);
        break;
    case SD_T_CHAIN_SHORT:
    case SD_T_CHAIN_LONG:
        if (st->cell >= 0)
            snprintf(out, n, _("Si la celda marcada no fuera %d, siguiendo la cadena terminaría siéndolo: es un %d."),
                     st->digit, st->digit);
        else
            snprintf(out, n, "%s", _("Seguí la cadena: en cada línea llena, uno de los dos es cierto; en cada punteada, no "
                               "pueden serlo los dos. Una de las dos puntas es cierta, así que lo tachado sale."));
        break;
    default:
        out[0] = 0;
        break;
    }
}
