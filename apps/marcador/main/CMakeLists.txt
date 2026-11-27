# The .so takes every .c under main/ (project_so's GLOB_RECURSE); the list is
# for the component.
idf_component_register(
    SRCS "marcador.c" "mr_model.c" "mr_px.c" "mr_snd.c" "mr_thumb.c" "mr_pano.c" "mr_cuenta.c" "mr_porotos.c" "mr_estadio.c" "mr_cartel.c" "mr_abaco.c" "mr_tiza.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
