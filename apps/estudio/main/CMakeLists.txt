idf_component_register(
    SRCS "estudio.c" "es_dsp.c" "es_engine.c" "es_patch.c" "es_proj.c" "es_file.c" "es_midi.c"
         "es_ui.c" "es_ui_play.c" "es_ui_seq.c" "es_ui_sound.c" "es_ui_song.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)

# The apps build with the size optimisation; the audio thread's code does
# not: the sound is rendered from there 1500 times a second, so the DSP
# and the engine get -O2, like the Tuner's analysis (apps/afinador), and
# the rest of the app stays small.
set_source_files_properties(es_dsp.c es_engine.c
    PROPERTIES COMPILE_OPTIONS "-O2"
    DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
