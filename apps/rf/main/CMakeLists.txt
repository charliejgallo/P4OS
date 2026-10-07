# librtlsdr (rtlsdr/) and its port (port/) are NOT listed here: the .so takes
# every .c under main/ through project_so()'s GLOB_RECURSE. This list is the
# normal component build's, which only needs to compile the app.
idf_component_register(
    SRCS "rf.c"
    INCLUDE_DIRS "." "rtlsdr" "port"
    REQUIRES lvgl
)
