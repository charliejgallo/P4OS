idf_component_register(
    SRCS "mesa.c" "mg_games.c" "mg_art.c" "mg_view.c" "mg_play.c" "mg_hud.c" "mg_setup.c" "mg_audio.c"
         "mg_save.c" "mg_gfx.c" "mg_pak.c"
         "g_ajedrez.c" "g_ajrules.c" "g_ajsearch.c" "g_damas.c" "g_reversi.c" "g_cuatro.c" "g_ludo.c"
         "g_backgammon.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the engines and the pixel loops is a #pragma in those files: the
# .so is compiled by elf_loader.cmake with -Os, and per-file options do not
# reach it.
