idf_component_register(
    SRCS "plotter.c" "pl_parse.c" "pl_data.c" "pl_chart.c" "pl_cfg.c" "pl_sheets.c" "pl_csv.c" "pl_live.c" "pl_i18n.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
