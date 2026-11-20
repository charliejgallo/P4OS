idf_component_register(
    SRCS "bastion.c" "bs_menu.c" "bs_pak.c" "bs_gfx.c" "bs_art.c" "bs_world.c" "bs_data.c" "bs_game.c" "bs_scene.c"
         "bs_hud.c" "bs_audio.c" "bs_save.c" "bs_gallery.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the simulation and the pixel loops is a #pragma in those files:
# the .so is compiled by elf_loader.cmake with -Os, and per-file options do
# not reach it.
