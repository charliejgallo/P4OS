idf_component_register(
    SRCS "correo.c" "cr_util.c" "cr_mime.c" "cr_html.c" "cr_net.c" "cr_sync.c" "cr_store.c"
         "cr_ui.c" "cr_ui_msg.c" "cr_ui_compose.c" "cr_ui_acct.c" "cr_portal.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
