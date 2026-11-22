idf_component_register(
    SRCS "metegol.c" "mg_menu.c" "mg_scene.c" "mg_input.c" "mg_audio.c" "mg_art.c" "mg_kit.c" "mg_gfx.c"
         "mg_pak.c" "mg_game.c" "mg_ai.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the physics and the pixel loops is a #pragma in those files: the
# .so is compiled by elf_loader.cmake with -Os, and per-file options do not
# reach it.
