idf_component_register(
    SRCS "traductor.c" "td_cfg.c" "td_detect.c" "td_dict.c" "td_hist.c" "td_job.c" "td_lang.c"
         "td_nmt.c" "td_online.c" "td_pak.c" "td_spm.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
