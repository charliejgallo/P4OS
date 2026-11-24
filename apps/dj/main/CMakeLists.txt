idf_component_register(
    SRCS "dj.c" "dj_mp3.c" "dj_flac.c" "dj_track.c" "dj_ana.c" "dj_engine.c" "dj_meta.c"
         "dj_ui.c" "dj_wave.c" "dj_lib.c" "dj_midi.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)

# The apps build with the size optimisation; the threads that decode and
# mix do not: the audio thread runs 750 times a second and the decoders
# chew through whole tracks, so they get -O2, like Estudio's DSP. The rest
# of the app stays small.
set_source_files_properties(dj_mp3.c dj_flac.c dj_track.c dj_ana.c dj_engine.c dj_wave.c
    PROPERTIES COMPILE_OPTIONS "-O2"
    DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
