# P4OS — Plan maestro

Sistema operativo para la **Waveshare ESP32-P4-WIFI6-Touch-LCD-5** (720×1280,
MIPI-DSI, GT911, 32 MB PSRAM, WiFi 6 por C6). Heredero de AmoledOS, con dos
objetivos:

1. **Explorar todo lo que da el ESP32-P4**: pantalla grande, PPA, JPEG y H.264
   por hardware, audio full dúplex, USB HS, cámara y el header de 40 pines.
2. Ser **mi pantalla asistente de banco**: control de Home Assistant, terminal y
   programador serie a distancia, herramientas de bus, Modbus, y apps
   instalables que usan hardware extra conectado al header.

Documentos del plan:
- [HARDWARE.md](HARDWARE.md): la placa, riesgos y las pruebas de la Fase 2
- [EXPANSION.md](EXPANSION.md): el header, el perfil de pines y la arquitectura de módulos
- [UI.md](UI.md): la interfaz tipo iPhone, vertical y horizontal
- [APPS.md](APPS.md): qué migra de AmoledOS, qué no, y las apps nuevas

## Hoja de ruta (desde el 2026-10-05)

Publicadas: 0.8.0, 0.9.0 (host USB, seguridad del portal), 0.9.1 (joystick
en todos los juegos, lienzo retro por CPU). Lo que sigue:

| Versión | Qué trae | Quién | Estado |
|---|---|---|---|
| **0.9.2** | El Programador graba por el host USB (EN/BOOT por RTS/DTR, y la secuencia de la USB-Serial-JTAG para C3/C6/S3/H2); entrar al portal desde afuera con el *subnet router* de Tailscale de Home Assistant (sólo documentación) | firmware | grabación probada con un ESP32 y un ESP32-C3 |
| **0.10, "el taller"** | `aos_io_pwm_*` (LEDC y sigma-delta: el P4 no tiene DAC), `aos_io_ir_*` (RMT, leer y emitir crudo), `aos_io_twai_*` (CAN, con un transceptor de 3,3 V) | firmware | en curso |
| | Apps **PWM** (perilla, servo, patrones), **IR** (aprender, guardar, mandar; códigos de SmartIR en un pack), **CAN** (espía del bus, enviar, grabar) | una sesión de app cada una, cuando su API esté | esperan su API |
| | Apps **EEPROM** (24xx, 25xx, 93xx, con versiones y checksums) y **Dibujo** | sesiones de app, sin firmware | listas para arrancar |
| **0.11** | **VNC**: el visor como app que se instala (mouse y teclado USB), después el servidor (necesita firmware) | app, luego firmware | |
| sin fecha | Tailscale dentro de la placa: no hay cliente oficial para ESP32; WireGuard (componente para ESP-IDF) contra un servidor propio es la alternativa | investigar | |

Hardware para probar el taller: receptores y emisores IR, LEDs y servos
(hay); EEPROM 24LC (hay); transceptores CAN SN65HVD230 (a comprar: el CAN
queda escrito y probado en el modo de autoprueba del controlador, y el
ESP32-C3 será el segundo nodo).

## Decisiones de arranque (propuestas)

| # | Decisión | Por qué |
|---|---|---|
| D1 | **Fork del código de AmoledOS v0.9.1** en un repo nuevo `P4OS`, con un primer commit que diga de dónde sale. | Reusar 300+ commits de trabajo probado (HAL, cargador, i18n, portal, audio, red) sin arrastrar el historial del reloj. |
| D2 | **Se mantiene el prefijo `aos_`** en la HAL y la API de apps. | Las apps se portan con diffs chicos y los arreglos se pueden pasar de un SO al otro. Renombrar 2400 símbolos no aporta nada. |
| D3 | **ABI nuevo: `AOS_ABI_VERSION` 100** (serie P4), con descriptor ampliado: orientaciones, `resize()`, badges, módulos requeridos, widgets. | Que una `.so` del reloj nunca cargue en la P4, y que el formato P4 pueda crecer sin chocar. |
| D4 | **ESP-IDF 5.5.5** (la que ya está instalada) + `esp_hosted` 1.4.* + `esp_wifi_remote` 0.14.*. IDF 6 no por ahora. | Es la única combinación que Waveshare valida con hardware; con IDF 6 sólo compila. |
| D5 | **LVGL 9.5.0** fijo (el mismo del reloj y del BSP). **La configuración se congela al final de la Fase 2.** | La config de LVGL es ABI: `LV_USE_PPA`, las draw units o `LV_USE_OS` cambian `lv_global_t` y rompen todas las `.so`. Primero se mide, después se congela, y recién ahí se compilan apps. |
| D6 | **Port de pantalla propio** (como en AmoledOS), con el BSP y `esp_lcd_hx8394` sólo para el init del panel. Rotación por PPA en el flush. | El BSP usa `esp_lvgl_adapter` 0.6 (pre-release) y trae el PPA apagado; necesitamos controlar el flush para rotar, escalar el lienzo retro y blittear. Se compara con el adapter en la prueba 5 antes de decidir. |
| D7 | **La HAL declara capacidades** (`aos_hal_caps()`: IMU, batería, PMU, cámara, enlace, USB host…). | La misma app sabe si hay IMU o no, sin `#ifdef`. |
| D8 | **Home Assistant por WebSocket API** (token de larga duración) para leer y controlar, **más MQTT Discovery** para que la P4 aparezca como dispositivo (brillo, notificaciones, sensores de los módulos). | Por WebSocket se reciben los cambios de estado en vivo y se llaman servicios; por MQTT, HA ve y manda a la P4. Es lo mismo que ya usan `ha-claude-usage` y el gateway. |
| D9 | **Programador serie primero por el header** (`uart.b` con EN/BOOT); USB host sólo con hub alimentado. | El puerto OTG no da 5 V. |
| D10 | Código y docs públicos en inglés; interfaz en es/en/de desde el día uno (el sistema de AmoledOS). Docs internos y de plan en castellano. | Igual que AmoledOS. |
| D11 | **Nuestro propio shell**, no ESP-Brookesia. Brookesia (el "teléfono" de Espressif, ejemplo 11 de Waveshare) se mira como referencia de rendimiento. | Mantener el modelo de apps, los íconos AIC, el portal y el simulador que ya tenemos. |

## Fases

### Fase 0 — Pensar (ahora)
- [x] Relevar la placa: hardware, pines, riesgos (HARDWARE.md).
- [x] Relevar AmoledOS: arquitectura, 60 apps, qué es del reloj (APPS.md).
- [x] Diseñar la interfaz (UI.md) y el sistema de módulos (EXPANSION.md).
- [ ] Revisar este plan juntos y cerrar las preguntas abiertas (abajo).

### Fase 1 — Todo lo que se puede sin placa (hasta que llegue) — cerrada el 2026-09-28
La placa llegó el 2026-09-28. Lo que falta confirmar en ella está junto en
[PRUEBAS-PLACA.md](PRUEBAS-PLACA.md).

1. [x] **Repo y esqueleto**: base importada de AmoledOS v0.9.1, target `esp32p4`,
   dos perfiles de chip, particiones de 32 MB (2 OTA de 8 MB, FAT 12 MB,
   coredump 1 MB). **Compila** (`tools/build_fw.sh rev3_x`, 2,4 MB).
2. [x] **Firmware `p4bench`** listo para grabar (bench/README.md): las pruebas
   de HARDWARE.md salvo BLE (11) y USB (14); la 10 (ESP-NOW) quedó resuelta
   sin placa: esp_hosted 1.4.7 no lo trae. Compila en rev3/rev1 y en las
   tres variantes de LVGL. `tools/bench_run.py` lo maneja desde la Mac.
3. [x] **Simulador a 720×1280** (SDL, nuevo `sim/main.c`): zoom, tecla `r`
   para girar, guiones de toques/arrastres y capturas PNG (`P4_SIM_SCRIPT`),
   la misma captura que usa el portal.
4. [x] **HAL**: tamaño de pantalla en tiempo de ejecución, rotación,
   capacidades (`aos_hal_caps()`); stubs débiles generados para todo lo que
   la placa todavía no implementa (`tools/gen_hal_stubs.py`).
5. [x] **El shell**: barra de estado, inicio con páginas + dock + carpetas +
   widgets + globitos, modo edición (arrastrar, crear carpetas, guardar
   `menu.txt`), Centro de control, notificaciones con banners, gestos de
   borde, selector de apps y multitarea (hasta 4 apps vivas), rotación en
   vivo. Probado en el simulador.
6. [x] **Apps** (2026-09-28), todas redibujadas para 720×1280 y probadas en
   vertical y horizontal en el simulador:
   - internas: Calculadora, Terminal, Reloj (5 pestañas), Electrónica,
     **Ajustes** (nueva, estilo iOS; barra lateral en horizontal),
     **Bus** (I2C + registros + GPIO en vivo), Calendario, Conversor, Vida,
     Música, Fotos;
   - dinámicas: cotiz, clima, radio, recorder, afinador, burbujas, gemas,
     simon, dados, flappy, buscaminas, atasco.
   - Fuente del sistema: Inter (medium para texto, semibold para los nombres
     de los iconos).
   - En la placa (2026-09-28): imágenes (`aos_image_p4.c`: motor JPEG hasta
     4 MP, esp_new_jpeg escalando para las grandes, libpng, BMP; probado en
     la Mac con `tests/image_host/run.sh`) y audio (`aos_audio_p4.c`: bus fijo
     a 48 kHz en los dos sentidos, full dúplex). Falta medir en la placa:
     orden de colores del motor (lo detecta solo), calentamiento del
     amplificador, curva de volumen, ganancia del ES7210, CPU del MP3/AAC,
     latencia del parlante en streaming. Los JPEG progresivos no se leen.
   - `aos_hal` se enlaza entero (WHOLE_ARCHIVE): si no, los stubs débiles le
     ganaban a las implementaciones reales en la placa.
   - Para las `.so` en la placa (después de congelar LVGL): regenerar
     `aos_symbols.c` (`tools/gen_symbols.py`), el `nm` de RISC-V en
     `build_apps.sh` y `CONFIG_IDF_TARGET` en los `sdkconfig.defaults` de
     las apps.
   - Faltan traducciones de los textos nuevos (`tools/gen_lang.py`).
6b. [x] **Red, Home Assistant, Programador, portal y Modbus** (2026-09-28):
   - red en la placa: Wi-Fi por el C6 (esp_wifi_remote + hosted), SNTP,
     mDNS con el nombre elegido; `aos_tcp`/`aos_http` y un cliente
     WebSocket (`aos_ws.c`) comunes a la placa y al sim;
   - **Home Assistant** (`aos_ha.c` + app + widget), token por `ha.txt`
     o por el portal; `tools/fake_ha.py` para probar;
   - **Programador** (`components/aos_flasher`, esp-serial-flasher 2.1) con
     EN/BOOT por `aos_io_uart_lines`; `tools/fake_esp_rom.py` para probar;
   - **portal web** (`components/aos_portal`: servidor propio sobre sockets,
     página única): pantalla en vivo con toques, Wi-Fi, HA, Programador,
     archivos, ajustes y registro;
   - **Modbus** (`aos_modbus.c` + app): Riden, gateway DOMCOM y
     explorador; `tools/fake_modbus.py` para probar.
   - Sin probar en la placa: todo lo de red, el UART a 460800 por el header,
     EN/BOOT reales, TLS (sólo `ws://` y `http://` para HA).
6c. [x] **Banco** (2026-09-28, docs/BENCH.md): osciloscopio Rigol DS1000Z
   por SCPI (`aos_scope.c`, trazas en vivo en un lienzo, medidas, captura
   PNG), fuente Riden como servicio (`aos_riden.c`, el enlace de Modbus),
   registrador a CSV que sigue con la app cerrada (`aos_bench_log.c`) y la
   página Banco del portal con la traza en el navegador;
   `tools/fake_rigol.py` para probar. El generador UTG espera un hub USB
   alimentado. Falta medir en los equipos reales: trazas por segundo del
   Rigol, pausa entre comandos, `*OPC?` del autoscale, tamaño de la captura.
6d. [x] **MQTT, Red, Monitor, Archivos y Claude** (2026-09-28):
   - **MQTT** (`aos_mqtt.c` + app + página del portal, docs/MQTT.md):
     cliente 3.1.1 con QoS 0/1, árbol de tópicos, gráficos por campo JSON,
     publicar; `tools/fake_mqtt.py`;
   - **Red**: ping ICMP (TCP de respaldo), barrido de la /24 con adivinanza de
     equipos, puertos, analizador Wi-Fi y mDNS;
   - **Monitor**: CPU por núcleo, tareas de FreeRTOS, memoria por capacidad,
     Wi-Fi y tarjeta; `/api/sysmon`; `aos_stats.c` entra a la P4;
   - **Archivos**: explorador de la tarjeta con copiar/mover/borrar en un hilo,
     visor de texto y CSV con gráfico, y "abrir con" (`aos_ui_open_app_with`)
     para Fotos, Música y el Programador;
   - **Claude** (docs/CLAUDE-APP.md): el uso del plan desde el mismo endpoint que
     el `/usage` de Claude Code, con login OAuth propio de la placa desde el
     portal; `tools/fake_claude_api.py`. Sin probar contra Anthropic todavía.
   - De paso: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` (AOS_BSS_PSRAM no
     hacía nada), `P4_SIM_PREFS` para simuladores en paralelo, notificaciones
     de HA que se cierran en HA, Modbus 8E1/8O1/8N2, filtro hexa y avisos
     editables en la Terminal.
6e. [x] **Cámaras, Macro pad, Módulos y lienzo retro** (2026-09-28):
   - **Cámaras** (apps/camaras, docs/CAMERAS.md): mosaico y pantalla
     completa, MJPEG por el motor JPEG (`aos_hal_jpeg_*`) y H.264 por
     tinyh264, pestaña Frigate, fotos al álbum; `tools/fake_rtsp.py`,
     `fake_mjpeg.py`, `fake_frigate.py`. `aos_tcp.c`: 6 conexiones y reserva
     atómica de lugar.
   - **Macro pad** (docs/MACROPAD.md): botoneras con teclas, texto, guiones,
     HA, MQTT y apps; trackpad; editor en la placa y en el portal; HID por
     TinyUSB en el OTG (`aos_usb_p4.c`, sin probar).
   - **Módulos** (docs/MODULES.md): el header dibujado, modules.txt con
     editor y validación, drivers I2C con autodetección (BME280/BMP280,
     SHT3x/4x, AHT20, BH1750, INA219/226, ADS1115), emulados en el sim;
     página Expansión del portal. Falta DS18B20, WS2812 y relés.
   - **Lienzo retro** (docs/RETRO.md): `aos_retro_*` con escala por el PPA
     (bilineal según IDF: mirar en la placa; `retro_hw=0` usa la CPU) y
     controles del SO; arkanos, topos, cjump, g2043 y claudito portados.
   - Falta para la placa: el cargador de `.so` en RISC-V, `build_apps.sh`
     para el P4 y regenerar `aos_symbols.c` (las apps dinámicas sólo corren
     en el simulador por ahora).
7. [x] **Árbitro de pines y `modules.txt`** (`components/aos_io`), con el
   simulador mapeando `uart.*` a puertos serie de la Mac. La Terminal serie
   y Bus lo usan.
8. [x] **El shell en la placa**: `aos_hal_p4.c` con el panel DSI, LVGL, la
   rotación por PPA, el GT911, NVS, SD y hora. Compila; se prueba en la Fase 2.

## Estado al 2026-09-30

| Fase | Estado |
|---|---|
| 0, 1 | Cerradas (2026-09-28). |
| 2 — Pruebas de hardware | Casi cerrada: chip rev 1.3, perfil `rev1_3`, LVGL por DMA2D, esp_hosted con el C6 de fábrica. Lo que queda está en PRUEBAS-PLACA.md. |
| 3 — Núcleo en la placa | Hecha, detallada debajo de la tabla. |
| 4 — Apps de sistema | Hechas en el simulador y abiertas en la placa; faltan pruebas con equipos (PRUEBAS-PLACA.md). |
| 5 — Asistente de banco | Hecho en el simulador; falta probar contra la Riden, el Rigol, HA y Mosquitto reales. |
| 6 — Módulos | Drivers y apps hechos; faltan los chips reales. |
| 7 — Apps dinámicas | Todas las que tienen sentido sin IMU ni ESP-NOW están en la tarjeta, 40 apps. Monster Hop, Turbo, Mila y Golf tienen arte HD re-renderizado. |
| 8 — El techo del P4 | En parte: Visor 3D con 100 000 triángulos en los dos núcleos, video 720p, Doom a 34 fps. |

Lo hecho en la Fase 3:
- HAL: panel DSI con 3 framebuffers y cambio de búfer, PPA/DMA2D, GT911 con
  dos dedos, SD, audio, Wi-Fi por el C6, NVS, SNTP, mDNS y coredump.
- Cargador `.so` RISC-V desde PSRAM.
- Portal completo (`#inicio`, `#pantalla`, Archivos…).
- Pantalla de arranque.
- Reparto de un cuadro entre los dos núcleos.

### Fase 2 — Pruebas de hardware (llega la placa)
- Grabar `p4bench`, correr las 21 pruebas, llenar la columna "medido".
- **Congelar** 🔒: perfil de chip, configuración de LVGL (ABI), método de
  rotación y framebuffers, perfil de pines por defecto, versión del firmware del C6.
- Salida: HARDWARE.md completo con las mediciones + `docs/HARDWARE.md` público.

### Fase 3 — Núcleo en la placa
HAL de la P4 (pantalla DSI + PPA, GT911, SD, audio full dúplex, WiFi por C6,
NVS, OTA, mDNS, SNTP, coredump), cargador ELF RISC-V (versión nueva de
`elf_loader` con el SoC del P4) + tabla de símbolos, el shell corriendo en la
placa, portal web (con `/pantalla` en vivo por JPEG por hardware y toque remoto),
herramientas (`captura.py`, `install_fw.sh`, `install_apps.sh`, `build_apps.sh`).
Mismo resultado en placa y simulador.

### Fase 4 — Apps de sistema e internas
Ajustes (reescrito), Archivos, Gestor de apps, Reloj (5 en 1), Calendario,
Calculadora, Conversor, Música, Fotos, Life, Pato goma, Monitor del sistema,
protectores de pantalla (las esferas).

### Fase 5 — El asistente de banco (lo que le da sentido al aparato)
Home Assistant (app + widgets + escenas + notificaciones), Terminal serie como
servicio, Programador (+ puente RFC2217), Herramientas de bus, Calculadoras de
electrónica, Modbus, MQTT, Herramientas de red, Consumo de Claude.

### Fase 6 — Módulos de expansión
Drivers en firmware (RS485, sensores I2C con autodetección, DS18B20, WS2812,
relés), `/expansion` en el portal, Ajustes → Expansión con el header dibujado,
la primera app **externa** que trae su propio driver (LoRa SX1262), y la guía
"cómo escribir una app que usa el header".

### Fase 7 — Migración de las apps dinámicas
En las 4 olas de APPS.md: primero las que re-maquetan, después las del lienzo
retro, después worker + assets re-renderizados, y al final las del enlace.

### Fase 8 — Explorar el techo del P4
Cámara CSI + H.264 por hardware (si es el modelo "-C"), asistente de voz HA
Assist con cancelación de eco, pantalla USB para la Mac, macro pad, el generador UTG del
Banco (con hub USB alimentado), analizador lógico por PARLIO, CAN.

## Riesgos principales (y plan B)

| Riesgo | Si pasa… |
|---|---|
| El C6 de fábrica no anda bien con esp_hosted | Grabar el esclavo de esp_hosted 1.4 en el C6 (islas J7 con adaptador USB-UART, o OTA desde el P4). |
| ESP-NOW / BLE no pasan por el C6 | Enlace por UDP en la LAN (el simulador ya lo hace); los relojes también lo tendrían que aprender. |
| LVGL lento a 720×1280 (5,6× los píxeles del reloj) | PPA como draw unit, 2 draw units, dirty areas, desenfoque falso, animaciones que empujan poco. Se mide en la prueba 5 antes de diseñar de más. |
| Horizontal más lento que vertical | Rotar en el flush con el PPA, o con DMA2D. Prueba 6. |
| Reubicaciones RISC-V que el cargador no conoce | Actualizar `elf_loader` al último de Espressif; `hello.so` y una `.so` grande en la prueba 19. |
| Revisión del chip distinta de la esperada | Dos perfiles desde el día uno. |
| GPIO46-48 a una tensión rara | Sacarlos del perfil por defecto; LoRa usa los sueltos (28/32/34). |

## Preguntas abiertas (para cerrar antes de la Fase 1)

1. ¿Qué modelo compraste: **estándar** o **"-C" con cámara OV5647**?
2. ¿Vas a tener a mano **parlante** (8 Ω, conector GH1.25), **batería** Li-ion
   (MX1.25) y **pila de RTC**? Sólo cambia qué pruebas se hacen el primer día.
3. ¿Tenés un **adaptador USB-UART de 3,3 V** suelto? Es el plan B del C6, y
   además sirve para probar la Terminal serie en el simulador.
4. ¿Tenés un **hub USB alimentado** o un cable OTG con inyección de energía?
   Decide si el USB host entra en la Fase 2.
5. ¿Los nombres **P4OS** para el repo y **`p4os.local`** para mDNS te quedan bien?
6. ¿Algún **módulo concreto** que ya tengas para el header (qué placa RS485, qué
   LoRa, qué sensores)? Con eso el perfil de pines se arma sobre hardware real.
