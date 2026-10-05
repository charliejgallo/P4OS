cmake_minimum_required(VERSION 3.16)
project(p4os_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
# The camera viewer's decoders (branch rtsp): H.264 and JPEG through
# libavcodec, behind the same interfaces the board has (sim/sim_codec.c).
#   brew install ffmpeg
pkg_check_modules(AVCODEC REQUIRED libavcodec libavutil libswscale)

# mbedtls para el https:// de aos_http.c. Es la MISMA biblioteca que usa la
# placa (el IDF trae 3.6.7, Homebrew 3.4.1) y por eso el cliente sigue siendo
# uno solo: lo unico que cambia entre plataformas es de donde sale el almacen
# de CAs, y eso son cinco lineas adentro de trust_attach().
#   brew install mbedtls
find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h
          HINTS /opt/homebrew/opt/mbedtls/include /usr/local/opt/mbedtls/include)
find_library(MBEDTLS_LIB     mbedtls     HINTS /opt/homebrew/opt/mbedtls/lib /usr/local/opt/mbedtls/lib)
find_library(MBEDX509_LIB    mbedx509    HINTS /opt/homebrew/opt/mbedtls/lib /usr/local/opt/mbedtls/lib)
find_library(MBEDCRYPTO_LIB  mbedcrypto  HINTS /opt/homebrew/opt/mbedtls/lib /usr/local/opt/mbedtls/lib)
if(NOT MBEDTLS_INCLUDE_DIR OR NOT MBEDTLS_LIB)
    message(FATAL_ERROR "falta mbedtls: brew install mbedtls")
endif()

# LVGL toma la configuracion de ../sim/lv_conf.h (ruta relativa a lvgl/src)
# LVGL incluye <SDL2/SDL.h>, asi que necesita el directorio padre del include
foreach(dir ${SDL2_INCLUDE_DIRS})
    get_filename_component(_parent ${dir} DIRECTORY)
    include_directories(${dir} ${_parent})
endforeach()
link_directories(${SDL2_LIBRARY_DIRS})

# Los ejemplos y demos de LVGL, apagados. Las variables que MANDAN son estas
# dos: os_desktop.cmake las declara con option(... ON) y no mira ninguna
# LV_CONF_BUILD_DISABLE_*, asi que las tres lineas que habia antes no apagaban
# nada y el simulador venia compilando demos y ejemplos en cada build. Se noto
# recien al sacar las Montserrat de LVGL: lv_example_style_21.c las nombra
# directo y fue lo unico que no compilo.
#
# FORCE porque option() no pisa una entrada de cache que ya exista.
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL 1 CACHE INTERNAL "")
add_subdirectory(lvgl)
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})

file(GLOB AOS_UI_SRC   CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_ui/*.c)
file(GLOB AOS_APPS_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_apps/*.c)

# Generated font set (tools/gen_fonts.py). Same sources as the board, so the
# simulator renders accented text exactly the way the hardware does.
file(GLOB AOS_FONT_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_fonts/*.c)

# Las apps dinamicas de apps/ tambien se compilan adentro del simulador, para
# poder disenarlas sin la placa. AOS_SIM_BUILTIN hace que se auto-registren en
# lugar de exportar el simbolo de entrada del .so.
# P4OS: only the apps already ported to the 720x1280 screen (APPS.md); the
# others still assume 368x448 and would not compile against the new HAL.
if(NOT DEFINED P4OS_SIM_APPS)
    # every app in apps/ but the blit bench (it only measures the board);
    # -DP4OS_SIM_APPS="a;b" builds a smaller simulator
    set(P4OS_SIM_APPS hello_app cotiz clima burbujas simon dados flappy gemas buscaminas atasco radio recorder afinador
        arkanos topos cjump g2043 claudito camaras pixel neon blackjack truco lua video doom chatarra
        visor3d mapas monsterhop turbo mila golf notas)
endif()
set(AOS_DYNAPP_SRC "")
foreach(a ${P4OS_SIM_APPS})
    file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../apps/${a}/main/*.c)
    list(APPEND AOS_DYNAPP_SRC ${_src})
endforeach()

# El interprete de apps/lua vive un nivel mas abajo que ese glob, que no es
# recursivo. Se compila como biblioteca aparte y no dentro del ejecutable
# porque es codigo de terceros: no le caben nuestras -Wall -Wextra (Lua sale
# con decenas de avisos que no vamos a arreglar) ni nos interesa releerlos en
# cada build. En la placa no hace falta nada de esto: project_so() junta el
# arbol entero con GLOB_RECURSE.
file(GLOB AOS_LUA_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../apps/lua/main/lua/*.c)
if(AOS_LUA_SRC)
    add_library(aos_lua STATIC ${AOS_LUA_SRC})
    # PUBLIC: lua_app.c y lua_init.c entran por el glob de arriba, o sea que
    # los compila el ejecutable, y necesitan ver los headers y el LUA_32BITS.
    target_include_directories(aos_lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../apps/lua/main/lua)
    target_compile_definitions(aos_lua PUBLIC LUA_32BITS=1)
    target_compile_options(aos_lua PRIVATE -w)
endif()

# Doom's engine (apps/doom/main/doomgeneric, GPL, third-party) and its port
# live below the glob too, and for the same reasons as Lua they build apart:
# Chocolate Doom does not survive our warnings, and the port's files see the
# engine's headers, which reroute malloc and exit (port/dg_compat.h) - that
# must not leak into the rest of the simulator.
file(GLOB AOS_DOOM_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/doom/main/doomgeneric/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/doom/main/port/*.c)
if(AOS_DOOM_SRC)
    add_library(aos_doom STATIC ${AOS_DOOM_SRC})
    target_include_directories(aos_doom PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/doom/main/doomgeneric
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/doom/main/port
        ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/include)
    target_compile_definitions(aos_doom PRIVATE AOS_SIM)
    target_compile_options(aos_doom PRIVATE -w)
endif()

# cJSON for the Home Assistant service: the board takes it from ESP-IDF's
# json component, so the simulator compiles the same file from the same place.
if(DEFINED ENV{IDF_PATH})
    set(AOS_CJSON_DIR $ENV{IDF_PATH}/components/json/cJSON)
else()
    set(AOS_CJSON_DIR $ENV{HOME}/esp/esp-idf/components/json/cJSON)
endif()
add_library(aos_cjson STATIC ${AOS_CJSON_DIR}/cJSON.c)
target_include_directories(aos_cjson PUBLIC ${AOS_CJSON_DIR})
target_compile_options(aos_cjson PRIVATE -w)

# The web portal, served by the simulator on port 8080 (P4_SIM_PORTAL_PORT).
set(AOS_WEB_FILES ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/web/index.html
                  ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/web/app.js
                  ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/web/app.css
                  ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_web/glifos.js)
set(AOS_WEB_C ${CMAKE_CURRENT_BINARY_DIR}/aos_portal_assets.c)
add_custom_command(OUTPUT ${AOS_WEB_C}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_web_assets.py ${AOS_WEB_C} ${AOS_WEB_FILES}
    DEPENDS ${AOS_WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_web_assets.py)

add_executable(p4os_sim
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_httpd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_bench.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_access.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_claude.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_mqtt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_sysmon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_system.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_macropad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/aos_portal_modules.c
    ${AOS_WEB_C}
    main.c
    hal_sim.c
    dirscan_sim.c
    tasks_sim.c
    nettools_sim.c
    sim_codec.c
    sim_aac.c
    retro_sim.c
    jpeg_sim.c
    usb_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_hid_keys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_http.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_ws.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_tls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_logring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_notif.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_device_name.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_link.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/aos_hal_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/aos_io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/aos_io_wire.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/aos_leds.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/aos_serial.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/aos_sensors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/aos_modules.c
    io_sim.c
    sensors_sim.c
    eeprom_sim.c
    ${AOS_UI_SRC}
    ${AOS_APPS_SRC}
    ${AOS_FONT_SRC}
    ${AOS_DYNAPP_SRC}
)

target_include_directories(p4os_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_hal
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_apps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_fonts/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_io/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_portal/include
    ${SDL2_INCLUDE_DIRS}
    ${MBEDTLS_INCLUDE_DIR}
    ${AVCODEC_INCLUDE_DIRS}
)
# esp_new_jpeg's headers only: sim_codec.c implements them on libavcodec. They
# come with the firmware's managed components (git-ignored), so a clone that
# never configured the firmware builds the simulator without the JPEG half;
# the code checks with __has_include.
set(AOS_ESP_JPEG_INC ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_new_jpeg/include)
if(EXISTS ${AOS_ESP_JPEG_INC}/esp_jpeg_dec.h)
    target_include_directories(p4os_sim PRIVATE ${AOS_ESP_JPEG_INC})
endif()

target_compile_options(p4os_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(p4os_sim PRIVATE AOS_SIM_BUILTIN AOS_SIM)
target_link_libraries(p4os_sim PRIVATE lvgl ${SDL2_LIBRARIES} m aos_cjson
    $<$<BOOL:${AOS_LUA_SRC}>:aos_lua>
    $<$<BOOL:${AOS_DOOM_SRC}>:aos_doom>
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB} ${AVCODEC_LIBRARIES})
target_link_directories(p4os_sim PRIVATE ${SDL2_LIBRARY_DIRS} ${AVCODEC_LIBRARY_DIRS})

# The Programador's service (components/aos_flasher) and its engine,
# Espressif's esp-serial-flasher. The library comes with the firmware's
# managed components (git-ignored, like esp_new_jpeg above): its own plain
# CMake build, with the port left to us (aos_flasher.c talks aos_io), quiet
# because it is third-party. Without it aos_flasher.c compiles to a stub that
# says a firmware build is needed first.
target_sources(p4os_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_flasher/aos_flasher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_flasher/aos_flasher_src.c)
target_include_directories(p4os_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../components/aos_flasher/include)
set(AOS_ESF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp-serial-flasher)
if(EXISTS ${AOS_ESF_DIR}/CMakeLists.txt)
    set(PORT USER_DEFINED)
    add_subdirectory(${AOS_ESF_DIR} esp-serial-flasher EXCLUDE_FROM_ALL)
    target_compile_options(flasher PRIVATE -w)
    target_link_libraries(p4os_sim PRIVATE flasher)
endif()
