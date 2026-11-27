# The .so takes every .c under main/ (pb/ included) through project_so()'s
# GLOB_RECURSE; this list is the normal component build's, which only needs
# to compile.
idf_component_register(
    SRCS "malla.c"
    INCLUDE_DIRS "." "pb"
    REQUIRES lvgl
)
