idf_component_register(
    SRCS "infrarrojo.c" "ir_base.c" "ir_hw.c" "ir_json.c" "ir_learn.c" "ir_pack.c"
         "ir_portal.c" "ir_proto.c" "ir_remote.c" "ir_store.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
