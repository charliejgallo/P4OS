idf_component_register(
    SRCS "cielo.c" "cl_view.c" "cl_draw.c" "cl_cat.c" "cl_names.c" "cl_conf.c" "cl_sat.c" "cl_ui.c"
         "cl_astro.c" "cl_sgp4.c" "cl_dmath.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the engine and the frame's pixel loops is a #pragma in each of
# those files: the .so is compiled by elf_loader.cmake with -Os, and
# per-file options do not reach it.
