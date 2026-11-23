idf_component_register(
    SRCS "tanques.c" "tq_battle.c" "tq_menu.c" "tq_hangar.c" "tq_game.c" "tq_data.c" "tq_scene.c" "tq_gfx.c"
         "tq_terrain.c" "tq_art.c" "tq_mem.c" "tq_snd.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the pixel loops is a #pragma in each of those files: the .so is
# compiled by elf_loader.cmake with -Os, and per-file options do not reach it.
