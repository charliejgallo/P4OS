idf_component_register(
    SRCS "notas.c" "nt_doc.c" "nt_editor.c" "nt_fonts.c" "nt_kb.c" "nt_store.c"
         "nt_tasks.c" "nt_theme.c" "nt_ui.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
