idf_component_register(
    SRCS "dibujo.c" "dib_editor.c" "dib_panels.c" "dib_icons.c"
         "dib_doc.c" "dib_paint.c" "dib_raster.c" "dib_png.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
