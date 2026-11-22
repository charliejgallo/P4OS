idf_component_register(
    SRCS "bolsa.c" "bs_cfg.c" "bs_chart.c" "bs_detail.c" "bs_i18n.c" "bs_list.c" "bs_net.c" "bs_prov.c" "bs_series.c" "bs_sym.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
