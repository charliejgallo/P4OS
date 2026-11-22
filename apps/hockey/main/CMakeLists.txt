idf_component_register(
    SRCS "hockey.c" "hk_menu.c" "hk_scene.c" "hk_input.c" "hk_audio.c" "hk_art.c" "hk_gfx.c"
         "hk_pak.c" "hk_game.c" "hk_ai.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the physics and the pixel loops is a #pragma in those files: the
# .so is compiled by elf_loader.cmake with -Os, and per-file options do not
# reach it.
