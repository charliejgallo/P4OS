idf_component_register(
    SRCS "vuelo.c" "vu_world.c" "vu_render.c" "vu_plane.c" "vu_flight.c" "vu_hud.c" "vu_mem.c"
         "vu_game.c" "vu_marks.c" "vu_text.c" "vu_snd.c" "vu_menu.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the pixel loops is a #pragma in each of those files: the .so is
# compiled by elf_loader.cmake with -Os, and per-file options do not reach it.
