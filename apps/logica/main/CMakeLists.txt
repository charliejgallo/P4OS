# The .so takes every .c under main/ through project_so()'s GLOB_RECURSE;
# this list is the normal component build's, which only compiles the app.
idf_component_register(
    SRCS "la_app.c" "la_cap.c" "la_dec.c" "la_demo.c" "la_dev.c" "la_fmt.c" "la_fx2.c" "la_live.c" "la_sr.c"
         "la_sump.c" "la_tinfl.c" "la_ui.c" "la_view.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
