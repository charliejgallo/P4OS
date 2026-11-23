idf_component_register(
    SRCS "asteroides.c" "ast_menu.c" "ast_scene.c" "ast_input.c" "ast_audio.c" "ast_bg.c" "ast_gfx.c"
         "ast_font.c" "ast_game.c" "ast_bot.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the physics and the pixel loops is a #pragma in those files: the
# .so is compiled by elf_loader.cmake with -Os, and per-file options do not
# reach it.
