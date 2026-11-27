idf_component_register(
    SRCS "mensajes.c" "mj_api.c" "mj_crypto.c" "mj_img.c" "mj_mtp.c" "mj_port.c" "mj_store.c" "mj_tinfl.c"
         "mj_tl.c" "mj_ui_chat.c" "mj_ui_list.c" "mj_ui_login.c" "mj_ui_util.c" "mj_web.c" "tl_schema.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
