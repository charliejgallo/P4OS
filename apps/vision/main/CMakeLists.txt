# The app's own files (the .so's compile step globs every .c under main/,
# quirc included). The engine, C++, is a module of its own: ../engine.
idf_component_register(
    SRCS "vision.c" "vi_run.c" "vi_src.c" "vi_pack.c" "vi_codes.c" "vi_names.c"
         "quirc/quirc.c" "quirc/decode.c" "quirc/identify.c" "quirc/version_db.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
