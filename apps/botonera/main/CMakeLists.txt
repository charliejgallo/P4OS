idf_component_register(
    SRCS "botonera.c" "bn_eng.c" "bn_dec.c" "bn_synth.c" "bn_grid.c" "bn_ui.c" "bn_edit.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)

# The apps build with the size optimisation; the mixer (750 blocks a
# second), the decoders and the synthesiser do not: they get -O2, like DJ's
# and Estudio's. The rest of the app stays small.
set_source_files_properties(bn_eng.c bn_dec.c bn_synth.c
    PROPERTIES COMPILE_OPTIONS "-O2"
    DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
