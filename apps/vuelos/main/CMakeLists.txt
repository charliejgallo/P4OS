idf_component_register(
    SRCS "vuelos.c" "vl_frame.c" "vl_feed.c" "vl_conf.c" "vl_air.c" "vl_ui.c" "vl_mvt.c" "vl_draw.c" "vl_render.c" "vl_store.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the decoder, the renderer and the frame's pixel loops is a #pragma
# in each of those files: the .so is compiled by elf_loader.cmake with -Os,
# and per-file options do not reach it.
