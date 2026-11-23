# The .so takes every .c under main/ (project_so's GLOB_RECURSE); Lua's tree
# is in ../lua on purpose and comes in through tg_lua.c only.
idf_component_register(
    SRCS "tortuga.c" "tg_menu.c" "tg_play.c" "tg_view.c" "tg_blocks.c" "tg_edit.c" "tg_lua.c" "tg_audio.c"
         "tg_world.c" "tg_logo.c" "tg_levels_data.c" "tg_gfx.c" "tg_pak.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
