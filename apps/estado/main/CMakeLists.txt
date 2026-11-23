idf_component_register(
    SRCS "estado.c" "ec_cfg.c" "ec_check.c" "ec_cert.c" "ec_hist.c" "ec_i18n.c"
         "ec_ui.c" "ec_list.c" "ec_detail.c" "ec_edit.c" "ec_desk.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
