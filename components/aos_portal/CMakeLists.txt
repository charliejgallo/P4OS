# P4OS web portal: the HTTP server, the API and the page, gzipped into C at
# build time by tools/gen_web_assets.py (the same step as the simulator's).
set(AOS_WEB_FILES ${CMAKE_CURRENT_LIST_DIR}/web/index.html
                  ${CMAKE_CURRENT_LIST_DIR}/web/app.js
                  ${CMAKE_CURRENT_LIST_DIR}/web/app.css
                  ${CMAKE_CURRENT_LIST_DIR}/../aos_web/glifos.js)   # folder glyphs as SVG paths, for #inicio
set(AOS_WEB_C ${CMAKE_CURRENT_BINARY_DIR}/aos_portal_assets.c)

idf_component_register(
    SRCS "aos_httpd.c" "aos_portal.c" "aos_portal_bench.c" "aos_portal_claude.c" "aos_portal_mqtt.c" "aos_portal_sysmon.c" "aos_portal_macropad.c" "aos_portal_modules.c" "aos_portal_system.c" "aos_portal_radio.c" "aos_portal_net.c" "aos_portal_access.c" ${AOS_WEB_C}
    INCLUDE_DIRS "include"
    REQUIRES aos_hal
    PRIV_REQUIRES aos_ui aos_apps aos_io aos_flasher json lvgl app_update esp_app_format fatfs
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    add_custom_command(OUTPUT ${AOS_WEB_C}
        COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_web_assets.py ${AOS_WEB_C} ${AOS_WEB_FILES}
        DEPENDS ${AOS_WEB_FILES} ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_web_assets.py
        COMMENT "Packing the portal's page")
    add_custom_target(aos_portal_assets DEPENDS ${AOS_WEB_C})
    add_dependencies(${COMPONENT_LIB} aos_portal_assets)
endif()
