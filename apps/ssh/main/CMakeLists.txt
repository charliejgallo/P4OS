idf_component_register(
    SRCS "sc_app.c" "sc_conn.c" "sc_term.c" "sc_view.c" "sc_kbd.c" "sc_store.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
