# P4OS: lo que falta confirmar en la placa

La placa llegó el 2026-09-28. Hasta ese día todo se escribió y se probó en el
simulador, contra servidores falsos en la Mac. Este archivo junta, en un solo
lugar, lo que **sólo la placa puede confirmar**, para ir tachando a medida que
se prueba.

## Estado al 2026-09-30 (lo de después, en los resúmenes de abajo)

- **La placa corre P4OS entero:** rev 1.3 del chip, perfil `rev1_3`, firmware
  por `tools/build_fw.sh rev1_3 … app-flash`, portal en `p4os.local`.
- **Arranque:** pantalla de arranque con el avance real y el sistema listo a los
  4,2 s con 40 apps de la tarjeta (04c5309).
- **Memoria:** todo lo que puede va a PSRAM: pilas, estáticos, esp_hosted,
  cJSON y mDNS. La RAM interna libre pasó de 46 a ~120 KB (`docs/MEMORY.md`).
- **Pantalla:** tres framebuffers del panel, con cambio de búfer para las apps
  (`aos_hal_display_back/flip/blit_into`). Además, reparto de un cuadro entre
  los dos núcleos con `aos_hal_worker_split`.
- **Apps en la tarjeta** (`docs/APPS-P4.md` tiene la tabla con los fps):
  - los juegos retro a pantalla completa;
  - Doom, Visor 3D (con modelos HD de 100 000 triángulos), Mapas, Video, Lua
    y Pixel Art;
  - Chatarra con arte 2×;
  - Monster Hop, Turbo, Mila y Golf con arte HD de Blender;
  - Blackjack, Truco y Neon Snakes.
- **Inicio:** carpetas completas (agregar, quitar, renombrar) y la página
  `#inicio` del portal para ordenarlo desde la computadora.
- **Firmware por Wi-Fi:** `tools/ota.sh` o la página **Firmware** del portal,
  con arranque a prueba y vuelta sola a la imagen anterior; el registro del
  arranque anterior y el volcado del último cuelgue se leen desde el portal
  (`docs/BUILDING.md`).

### Pendientes, en resumen

Hecho entre el 2026-10-04 y el 2026-10-05 (0.9.0):
- [x] **Host USB** (docs/USB.md): los dos puertos a la vez (21/23 y 25/27)
      con la copia propia de la librería host (`components/usb`); probados
      en la placa un pendrive (lectura 7,4 MB/s, escritura), teclado (también
      en Notas), mouse, joystick, hub, webcam C270 en Cámaras, placa de
      audio (salida y micrófono), y por serie CH340, CP2102, FTDI, un
      Arduino Leonardo (serie + teclado) y una Pico (programada con su .uf2
      desde la placa, REPL de MicroPython). El detalle de cada uno, abajo en
      **USB**. Sin probar: MIDI con un aparato real.
- [x] **Joystick en todos los juegos** (2026-10-05, 0.9.1,
      `docs/GAMEPAD.md`): los 22 juegos y Atrapa (Lua) probados en la placa
      con el joystick genérico (cruceta como ejes, sin hat), todos bien.
      Turbo giraba de golpe porque tomaba la cruceta por palanca a fondo
      (arreglado en aos_pad.h). Sin probar: las partidas entre dos placas
      (carrera de Monster Hop, visitas de Mila, Truco de a dos).
- [x] **Programador por el host USB** (2026-10-05, para la 0.9.2): un
      ESP32 de desarrollo con CP2102 en `usb0` entra solo en modo de
      descarga por RTS/DTR; detectado en 0,8 s y grabado con
      `tools/usb_test/hola_esp32` (200 KB a 460800, MD5 bien, 6,6 s). La
      Terminal en `usb0` leyó los "Hola mundo" y le mandó texto (volvió
      "Recibido"). Y un ESP32-C3 por su USB-Serial-JTAG (otra secuencia de
      DTR/RTS): detectado en 0,5 s, grabado en 1,5 s, leído en la Terminal.
      Su firmware viejo se reiniciaba cada 2,7 s: el Programador espera
      hasta 3 s a que el puerto vuelva.
- [x] **RF con la RTL-SDR** (2026-10-06, `docs/plan/RF.md`): en 25/27, a
      2,048 y 2,4 Msps (4,1 y 4,8 MB/s) sin muestras perdidas, 25 cuadros/s
      con 25-35 % de CPU; desenchufada y vuelta a enchufar con la app
      abierta, vuelve sola. Sin probar: 21/23 (Full Speed).
- [ ] **Apps del taller** (los cableados en `docs/img/bench-*.svg`, que arma
      `tools/bench_diagrams.py`: PWM, IR, tira WS2812B y 24LC;
      instaladas el 2026-10-05, probadas sólo en el
      simulador): EEPROM con una 24LC256 en i2c.ext (pines 15/17,
      pull-ups de 4,7 kΩ): detectada en 0x50 y el tamaño medido
      escribiendo da 24LC256 (2026-10-05); leída entera y guardada como
      versión, editada, escrita y verificada, restaurada a la original y
      vuelta a escribir: los CRC32/MD5 de las versiones coinciden con los
      archivos medidos en la Mac (original y restaurada 1B43EABD, la
      editada 39CE0FF3); PWM con LEDs (**anduvo
      el 2026-10-05**: un LED en GPIO28, a mano, rampa, respirar y estrobo)
      y un servo (**anduvo el 2026-10-05**, cableado como
      `docs/img/bench-pwm.svg`: tres frecuencias a la vez, 998 Hz, 5 kHz y
      50 Hz, más el nivel analógico en GPIO30, 1,34 V pedido y 1,313 V
      medidos con tester; el servo en GPIO32: los dos extremos de
      500 a 2500 us y el barrido; al apagar el canal se movía solo porque el
      pin quedaba con el pull-up, arreglado: el pin suelto queda tirado al
      nivel de apagado); Infrarrojo con receptor y emisor (**mandar anduvo
      el 2026-10-05**: receptor en GPIO5, LED IR con transistor en GPIO31,
      un código del monitor mandado, NEC a 38 kHz; cambiar los pines
      colgaba la placa, arreglado: el esquema de Cableado le pasaba a LVGL
      un texto de la pila sin `text_local`; aprender anduvo: un botón del
      control del monitor quedó como Samsung, dirección 0x0707, comando 7,
      y se volvió a mandar, y el monitor subió el volumen; con la base de
      SmartIR, "subir volumen" desde el listado también anduvo); CAN con los
      transceptores SN65HVD230 cuando lleguen (la autoprueba ya anduvo);
      Visor VNC contra una computadora (**anduvo el 2026-10-05** con una
      MacBook Air y dos monitores, Compartir pantalla con contraseña VNC,
      RFB 3.889, ZRLE: login escrito con la burbuja del teclado, mouse y
      teclado andando, 60 eventos de mouse y 40 de teclado en el registro.
      La Mac manda los dos monitores como una sola pantalla de 4860x2316 y
      no dice dónde está cada uno; el botón de monitor ofrece la lista del
      servidor si la manda y "Marcar con el dedo". Adivinar los monitores
      por el negro entre ellos cortó el escritorio real en 8 y se sacó.
      Una trampa que parece un fallo: con la pantalla de bloqueo, la Mac
      apaga la pantalla al minuto y sigue mandando la última imagen, así
      que los toques parecen no hacer nada; una tecla la despierta);
      Dibujo con el dedo y el mouse USB.
- [x] Red, pestaña Wi-Fi fluida (2026-10-06, `5362893`): los gráficos de
      canales y el nivel en vivo se dibujaban en cada cuadro del scroll;
      ahora se pintan una vez en una imagen en PSRAM cuando cambian los
      datos. Probado por el usuario en la placa: el scroll va fluido
      también sobre los gráficos. 5 y 6 GHz no: el ESP32-C6 es sólo 2,4 GHz.
- [x] El cuelgue del primer arranque tras algunas OTA (resuelto el
      2026-10-06): no era de las OTA, era de cualquier arranque (~1 de cada
      18), y sólo se veía en una OTA porque la vuelta atrás lo delataba. El
      cargador de apps sincronizaba TODA la caché con la ROM después de
      cada `.so` mientras el otro núcleo dibujaba la pantalla de arranque, y
      a veces rompía datos de la PSRAM durante el escaneo de apps. Medido
      con `tools/boot_loop.sh`: 4 caídas en 70 reinicios antes, 0 en 80
      después (sincronizando por rango sólo el bloque del módulo), y 0 en
      27 con la imagen de producción. Detalle en `docs/MEMORY.md`.
- [ ] Sin Wi-Fi después de un reinicio por software (visto otra vez el
      2026-10-06, 1 vez en ~140 reinicios seguidos): la interfaz anda, el
      C6 no levanta la red, y no lo arreglan ni apagar y prender el Wi-Fi
      ni los dos reinicios del vigilante de red; el botón de reset (que
      corta la alimentación) sí, al instante. Desde ahora el registro se
      guarda en `/sdcard/logs/sin-red-N.txt` antes de reintentar: la
      próxima vez, leerlo de la tarjeta.

Hecho el 2026-10-04 (0.7.0):
- [x] Bluetooth con el iPhone 15 Pro Max: aparece en Ajustes del iPhone
      (anuncia HID), empareja por comparación de números, notificaciones en
      avisos y bloqueo, batería y hora del teléfono.
- [x] Emojis en color (paquete Noto en /fonts/emoji.pak): avisos, centro de
      notificaciones y bloqueo, vistos por el usuario.
- [x] Modo Teclado Bluetooth con una MacBook Air y el iPhone a la vez: texto
      de 72 caracteres, subir volumen, trackpad del Macro pad (dice BLE).
      Los dos se reconectan solos después de reiniciar el Bluetooth.
- [x] Seguridad del portal (2026-10-04, docs/SECURITY.md): rechazo de
      pedidos de otros sitios y de otros nombres, redes de confianza,
      contraseña con sesión y «Cerrar sesión» (probados por el usuario).
- [x] HTTPS con la autoridad propia (2026-10-04): instalada como confiable
      en la Mac (llavero Sistema, `security add-trusted-cert -d -r
      trustRoot`), candado en Chrome visto por el usuario. Las páginas de
      las apps cargan por HTTPS (conexiones que esperan lugar, sesiones TLS
      reanudadas). Falta el iPhone.
      Trampas: el doble clic al .cer puede dar -25294 (no encuentra el
      llavero) o dejarlo importado sin confianza; `security dump-trust-settings
      -d` y `security verify-cert -p ssl -s p4os.local` dicen la verdad.
- [x] Páginas de Grabadora, Clima y Cotizaciones (2026-10-04, después de la
      0.8.0) y rangos en `/api/fs/get`: probadas en la placa por el
      usuario, con las apps abiertas.
- [x] Ajustes, Desarrollador (2026-10-04): toques y fps encima de todo, el
      nivel del registro, los ajustes de dibujo de `/api/tune` con su vuelta
      a fábrica, y reiniciar en modo seguro sin BOOT. Probados en la placa
      por el usuario los toques, los fps y el modo seguro pedido.
- [x] La música del iPhone (AMS) y las acciones de las notificaciones desde
      la interfaz (2026-10-04): centro de control, bloqueo, botones en el
      centro de notificaciones y pantalla de llamada entrante. Probado por el
      usuario con el iPhone: la música, una llamada atendida y una
      rechazada, y «Borrar en el teléfono» en un WhatsApp.
- [x] Páginas del portal enchufables (docs/PORTAL-PAGES.md): la de
      hello_app sale de /web de la tarjeta y aparece en el menú, probada por
      el usuario; una rota no tumba el portal (probado en el simulador).
- [x] Mudadas a sus apps las páginas que vivían en el firmware: Notas,
      Pixel Art, Lua, Mapas y el Visor 3D (`app.js` de 5300 a 2565 líneas).
      Revisadas una por una en el simulador; en la placa cargan de /web.
- [x] Páginas nuevas de Radio (`/api/radio`, las nueve teclas, buscar y
      arrastrar) y Cámaras (`/cameras.txt` con vista previa), desde sus apps
      (2026-10-04). Radio probada en la placa por el usuario: emisoras
      nuevas en las teclas. Cámaras, sólo en el simulador con la cámara
      falsa.
- [ ] Red guarda cada barrido en `/redes` (NDJSON, los 40 últimos) y el
      portal tiene la página Red: lanzar un barrido (`/api/net`), ver los
      guardados, cambios contra el anterior, gráfico de canales, CSV.
      Barrido real en la placa hecho por el usuario (2026-10-04): anda.
- [x] Las MAC en el barrido (tabla ARP leída tanda por tanda) y los equipos
      «sólo ARP»: vistas en la placa por el usuario (2026-10-04), con el
      fabricante de `/redes/oui.txt` (40.033 OUI, sacados del `manuf` de
      Wireshark porque la IEEE rechaza la descarga automática).

Hecho el 2026-10-03:
- [x] esp_hosted 3.0.9 en el P4 con el C6 de fábrica: Wi-Fi, AP (probado
      desde el celular), portal, OTA y el vigilante del enlace andan.
- [x] Respaldo de los 4 MB del C6 por J7 (pinzas, 115200) y el C6 actualizado
      a 3.0.9 desde el P4. También 3.0.9 → 2.12.13 → 3.0.9 y la vuelta al de
      fábrica con la app sacada del respaldo, todo sin cables.
- [x] Caudal: 32 buffers TX del C6 (desde el P4) o la bajada cae a 0,4 MB/s;
      con 32, 0,8–1,3 MB/s, igual que el de fábrica (docs/C6.md).
- [ ] La consola del C6 por J7 no mostró nada después de sacar el puente de
      IO9 (¿se movió la pinza del TX?). Sirve para el próximo corte.
- [x] Vigilante del enlace con el C6: tres consultas al C6 sin respuesta (o una
      trabada más de 20 s) vuelcan las tareas al registro y reinician. Probado
      en la placa con `POST /api/wifi/linktest` (congela `sdio_process_rx`):
      detecta a los 18 s y la red vuelve a los 28 s. Un C6 en reset lo ve
      solo esp_hosted (las escrituras SDIO fallan y reinicia en 30 ms).
      Pendiente: ver si la próxima OTA que se corte la recupera sola.

Hecho el 2026-09-30:
- [x] Traducciones completas en/de (3821 textos, `lang/`), instaladas con
      `tools/install_lang.sh`; la app en primer plano ahora carga su catálogo.
- [x] Páginas del portal `#3d`, `#mapas`, `#lua` y `#pixel` (09a5414).
- [x] Casita de Mila más grande, en los dos sentidos (1,92×, f0cabee).
- [x] OTA por el portal y `tools/ota.sh`: ~25 s para 5,7 MB, arranque a
      prueba confirmado a los 30 s y vuelta atrás probada con una imagen
      que se colgaba a propósito (la ranura quedó "aborted").
- [x] Registro del arranque anterior (64 KB, sobrevive un reinicio, no un
      corte de luz) y volcado del cuelgue descargable (`tools/coredump.sh`);
      Registro con filtro, colores y descarga.
- [x] Perro guardián de la red: si a los 60 s no conectó, reinicia (dos veces
      como mucho). La causa de que a veces no vuelva tras un reinicio por
      software sigue sin encontrarse.

Del sistema:
- [x] Juegos acostados: girar franjas de RAM interna con el PPA (hecho por la
      auditoría de RAM interna del 2026-09-30: Monster Hop toma dos franjas
      de 9 filas acostado, Mila las suyas), que necesita
      dos bloques de 20 KB internos por app. Acostada faltan los ~57 KB del
      búfer de rotación de LVGL (sólo existe así): la idea es que el HAL se
      lo preste al juego mientras se juega y lo recupere en los menús
      (medido: vertical 169 KB libres, acostada 112).
- [x] Un cambio de búfer para las apps que redibujan por partes (Mila):
      `aos_hal_display_back_age()` (011dc81) y Mila con un anillo de daños
      (5a05606). Parada, la casita cambia de búfer en todos los cuadros.
- [x] Mila acostada cambia de búfer (2026-09-30, después de la auditoría de
      RAM interna: el bloque DMA más grande pasó de 30 a 139 KB). Medido:
      franjas de 44 KB internas, mapa 139 cuadros por cambio de búfer y 0
      empujados, 46 fps, dibujo 11 ms; nivel 8-12 ms por cuadro, cambio de
      búfer ~20 µs. Antes: Mila acostada no cambiaba de búfer: caía sola al envío de siempre (0
      cambios, todo "pushed") y se ve bien. La causa: acostada quedan ~36 KB
      de RAM interna apta para DMA, en pedazos de 10,7 KB como máximo, y el
      PPA necesita franjas de 8 filas (20 KB). Para arreglarlo hay que ver
      quién usa la RAM interna DMA (búferes de LVGL 2 x 80 KB, Wi-Fi, audio,
      SD) y liberar un bloque. El bloque de 31 KB que muestra el monitor es la
      LP SRAM, que no sirve.
- [x] `AOS_APP_FLAG_KEEP_AWAKE` que dure mientras la app está al frente, con
      el apagado automático nuevo en Ajustes → Pantalla (e58844d).
- [x] `aos_ui_overlay()` para las apps que escriben en la pantalla: Monster Hop,
      Turbo, Doom, Video, Visor 3D, Mapas y Golf se pausan debajo de un
      panel, y Doom, Video, Visor 3D y Mapas cambian de búfer (sin cortes).
      Doom medido: 35 fps con cambio de búfer.
- [x] El servicio del lienzo retro: `LR_SPLIT`, dedos sobre controles, A/B
      (15ff279); Claude Jump usa ya los botones del servicio.
- [ ] Exportar `open`/`read`/`close` en la tabla de símbolos, que pidieron Mila
      y Golf.
- [ ] Chicos: `i2s_channel_disable … not enabled` al reabrir el códec; `curl
      p4os.local` sin `-4` espera 5 s por un AAAA; el teclado de LVGL sale
      claro sobre la hoja oscura (renombrar una carpeta).
- [ ] Simulador:
  - `LUAI_MAXCCALLS=100` para `aos_lua`, como en la placa (el simulador usa
    el valor de Lua);
  - que informe la PSRAM y los fps reales;
  - agregar las apps nuevas a `P4OS_SIM_APPS` (`sim/CMakeLists.txt`);
  - que llame a `aos_hal_sim_link_tick()`.
- [x] Claudito por filas que cambian (quieto ~3 % de un núcleo, antes ~15 %).
- [ ] 2043 a 30 fps: ~24 fps presentando sólo las baldosas que cambian (antes
      26 medido con otro decorado; 20,7 presentando todo). El techo es LVGL
      refrescando por tandas con el PPA: un camino directo al búfer libre
      necesitaría dibujar los botones dentro del lienzo.
- [x] Doom con cambio de búfer: 35 fps y sin corte posible entre cuadros.

Que necesitan la mano del usuario o equipos:
- [x] Pantalla de bloqueo (2026-10-03): probada por el usuario en la placa
      de reemplazo, con código; las traducciones sin tarjeta, en el
      simulador.
- [x] Tira WS2812B desde Tiras LED (2026-10-05): 12 LEDs en GPIO4 (pin 14),
      dato a 3,3 V directo con 330 Ω en serie (sin level shifter, anda),
      5 V del header, límite de fuente 500 mA. Orden GRB correcto (rojo,
      verde y azul sólidos salen bien). Los efectos a 50 fps (Rainbow
      cycle, ~128 mA estimados), y vuelve sola con su efecto después de
      reiniciar. Falta medir el limitador en blanco pleno con el tester.
- [ ] DS18B20 real en Bus → 1-Wire (pin 16, 4,7 kΩ a 3V3).
- [ ] RC522 en spi.a con el ejemplo de Bus (VersionReg 0x91/0x92).
- [x] SPI en el conector (2026-10-01): el lazo del pin 34 al 36 a 1, 10 y 40 MHz,
      los 16 bytes de vuelta (captura en el README). Con el puerto abierto,
      314 KB de interna libre. Falta un chip real: un RC522 (ejemplo nuevo
      en Bus, `44bbace`), y una memoria flash.
- [x] I2C con un BME280 real (2026-10-01): en i2c.ext (pines 15/17, 3V3 en
      el 18), Bus lo ve en 0x76 y Módulos lo detecta solo; 181 lecturas sin
      errores, 23,1 °C, 43 % y 1020 hPa. Fotos en el README.
- [x] Punto de acceso propio (2026-10-01): el C6 de fábrica hace SoftAP en
      APSTA; el usuario se unió con el QR desde el teléfono y abrió el portal
      con el otro. Con el AP prendido la RAM interna no cambió (291 KB) y la
      red de casa siguió igual (1,59 MB/s por Wi-Fi contra 1,7 de antes).
      Del teléfono a la placa por el AP: los 7,5 MB de mila_p4.pak en 2-3 s
      (~3 MB/s, cronometrado a mano: el doble que por el router). El
      contador de Ajustes y el registro vieron al teléfono entrar.
- [x] Ajustes nuevos (2026-09-30), en vertical y acostada (Almacenamiento y el QR probados por el usuario):
      Almacenamiento (la barra por tipo, el recuento de la tarjeta real,
      Expulsar y Montar), Actualización (las dos ranuras; "Volver a esta
      versión" pide dos toques y reinicia con la otra), Diagnóstico
      (temperatura, CPU, motivo del reinicio, modo seguro, volcado) y el QR
      del portal en Acerca de, leído con el teléfono.
- [x] El botón BOOT (2026-09-30): toque = inicio (y despierta la pantalla
      apagada), largo = captura en `/sdcard/photos/Capturas` (un álbum de
      Fotos), apretado durante la pantalla de arranque = modo seguro. Los
      tres probados por el usuario. Una vez, tras el reinicio de una OTA,
      el pin no dio ningún flanco hasta el RESET (`docs/BUILDING.md`, el
      vigilante de cuelgues).
- [x] El primer login real de Claude (anduvo a la primera).
- [x] USB como teclado, mouse y macro pad (2026-09-30): enumera en HS,
      los tres reports, despierta la Mac y la placa se alimenta por el OTG.
- [x] USB como disco (2026-09-30): la tarjeta en la Mac y de vuelta.
- [ ] Cámaras (timbre y exterior).
- [ ] Ganancia del micrófono (ES7210), calentamiento del amplificador y
      latencia.
- [ ] Riden por TTL y Rigol por la LAN, Terminal a 460800 y Programador contra
      un ESP32.
- [ ] HA con `wss://` real, Mosquitto real y módulos I2C reales.
- [ ] La prueba 17: la hora con una pila en H3.
- [ ] Jugar a mano en la placa lo que no se puede tocar por el portal: Mila y
      Golf leen el táctil del panel.

Apps del reloj que no se pasan (necesitan IMU o el enlace ESP-NOW): escáner,
laberinto, pong, radar, walkie, remoto y sensores.

## Punto de partida

- **Estado del repo:** 54 commits, hasta `94f04c6`.
- **Firmware:** compila con `tools/build_fw.sh rev3_x` y deja 42 % libre en la
  partición.
- **RAM interna:** 215 KB en uso de 512.
- **Hecho en la Fase 1** (el detalle está en PLAN.md, puntos 1 a 8):
  - el shell completo;
  - 23 apps portadas;
  - Ajustes, Home Assistant, Terminal, Programador, Bus, Modbus y Banco;
  - MQTT, Red, Monitor, Archivos, Claude, Cámaras, Macro pad y Módulos;
  - el lienzo retro con cinco juegos;
  - el portal web.
- **Documentación por tema:** `docs/BENCH.md`, `CAMERAS.md`, `CLAUDE-APP.md`,
  `HOME-ASSISTANT.md`, `MACROPAD.md`, `MODULES.md`, `MQTT.md` y `RETRO.md`.

## Orden

1. **Fase 2, el banco de pruebas** (`bench/README.md`, `docs/plan/HARDWARE.md`):
   - chip_id y respaldo completo del firmware de fábrica;
   - `p4bench`, las 21 pruebas y las variantes de LVGL;
   - congelar las decisiones marcadas con 🔒.
2. **P4OS en la placa** (Fase 3), en este orden:
   1. arranque y pantalla;
   2. táctil y rotación;
   3. tarjeta, NVS y hora;
   4. Wi-Fi por el C6;
   5. portal;
   6. el resto.
3. **Las apps**, con los equipos de verdad: la lista de abajo.

## Plataforma

- [x] **Qué chip vino:** revisión del chip (perfil `rev3_x` o `rev1_3`) y
      tamaño de la flash.
- [x] **Pantalla:**
  - [x] el panel DSI con LVGL;
  - [x] la rotación: por CPU a un buffer interno y DMA2D al panel (el PPA
        escribiendo PSRAM era 11 veces más lento);
  - [x] el doble framebuffer: queda uno solo (con dos, `esp_async_fbcpy`
        dibujaba basura); **cambiado el 2026-09-29**: tres framebuffers para
        que las apps cambien de uno a otro (`aos_hal_display_back/flip`),
        LVGL sigue copiando al que se ve;
  - [x] la variante de LVGL: lvB, 35 fps vertical y 22 horizontal.
- [x] **Táctil:** el GT911, con dos dedos (pinch, y scroll a dos dedos en el
      Macro pad). Probado por el usuario: pinch en Chatarra y Mila, pinch y
      giro con dos dedos en el Visor 3D (2026-09-29).
- [x] **Almacenamiento:** tarjeta SD (SDMMC), NVS, SNTP.
  - [x] SD (10,8 MB/s de lectura), NVS y SNTP con la zona horaria. No hay
        chip RTC en esta placa (HARDWARE.md): queda la prueba 17.
  - [x] Un montaje de la SD falló una vez tras un reinicio por software
        (2026-09-29): ahora se intenta tres veces (d2e1955).
- [ ] **Wi-Fi por el C6:**
  - [x] que esp_hosted funcione con el firmware de fábrica del C6 (en modo
        streaming, el de paquete corta el enlace);
  - [x] throughput y ventana TCP (2026-09-29): 1,1 MB/s de subida y 1,8
        de bajada por el portal, detalle en `docs/MEMORY.md`;
  - [ ] cuántos sockets entran a la vez: portal + HA + MQTT + cámaras;
    `aos_tcp` ahora tiene 6.
- [ ] **Audio:**
  - [ ] calentamiento del amplificador;
  - [ ] curva de volumen: la de esp_codec_dev (-50..0 dB, lineal) dejaba
        mudo el medio; ahora -36/-20/-10/-4/0 dB en 1/25/50/75/100, a
        confirmar de oído;
  - [ ] ganancia del ES7210;
  - [x] CPU que usa el decodificador MP3: 10-11 % de un núcleo con un MP3
        de 320 kbps a 44,1 kHz pasado a 48 (AAC sin medir);
  - [ ] latencia del parlante.
- [ ] **Imágenes:**
  - [x] el motor JPEG anda (una sonda real de 64x64 en lugar de la de 16x16
        que daba timeout, 02ddfb0): 640x360 en 3 ms;
  - [ ] el orden de colores del motor JPEG (se detecta solo);
  - [ ] la regla de relleno del motor;
  - [ ] la velocidad para fotos de 12 MP.
  - [x] Fotos en la placa (2026-09-29): 348 fotos y 3 álbumes, colores bien,
        PNG bien. El JPEG progresivo no abría (ni el motor ni esp_new_jpeg
        los decodifican): ahora va por stb_image, hasta 2,5 MP.
- [x] **PSRAM:** que `AOS_BSS_PSRAM` (con
      `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) no rompa nada, porque
      la tabla de apps y el servicio de HA viven ahí ahora.
  - [x] Pilas de los hilos, de esp_hosted, de mDNS y los estáticos de las
        apps en PSRAM (2026-09-29): la RAM interna libre pasó de 46,5 a
        ~117 KB. Las reglas están en `docs/MEMORY.md`.
  - [x] Preferencias escritas desde un hilo con pila en PSRAM (el portal),
        sin el assert de la flash.
  - [x] Recorrer las apps una por una con las pilas en PSRAM, mirando que no
        haya destellos ni reinicios: 16 apps seguidas sin reinicio (APPS-P4.md).
- [x] **Cargador de apps .so para RISC-V** (Fase 3), `build_apps.sh` para el
      P4 y `aos_symbols.c` regenerado (5ba450e, c04ef3c): 34 módulos y 40
      apps en la tarjeta (2026-09-30). Detalle en `docs/APPS-P4.md`.

## Por app

**Terminal y Programador**
- [ ] El UART a 460800 por el header.
- [ ] EN/BOOT reales para programar otra placa.
- [ ] esp-serial-flasher contra un ESP32 de verdad.

**Modbus y Banco**
- [ ] La Riden por TTL: los 20 ms entre pedidos y las escrituras que rechaza en
      silencio.
- [ ] El Rigol por la LAN:
  - [ ] trazas por segundo (en el simulador dio entre 11 y 16);
  - [ ] la pausa entre comandos;
  - [ ] que `*OPC?` espere al autoscale;
  - [ ] el tamaño y el tiempo de la captura PNG;
  - [ ] lo que le cuesta a LVGL dibujar el lienzo de la traza.

**Home Assistant**
- [ ] `wss://` con un certificado real.
- [ ] Cerrar una notificación en la P4 y ver que se cierra también en HA.

**MQTT**
- [ ] Un Mosquitto de verdad: usuario y contraseña, y los rechazos por ACL.
- [ ] Una ráfaga de mensajes.
- [ ] Que el Wi-Fi se corte y se reconecte.

**Red**
- [ ] ICMP por lwIP.
- [x] La tabla ARP (10 entradas) durante el barrido de la /24 (terminó bien
      el 2026-09-29, después de pasar los buffers de esp_hosted a PSRAM).
- [ ] mDNS con 15 búsquedas a la vez.
- [ ] Que el C6 pase el ancho de canal; el C6 es sólo 2,4 GHz.

**Monitor**
- [x] La carga por núcleo.
- [x] Pila y CPU por tarea (las pilas en PSRAM se miden de a una por vuelta).
- [x] El sensor de temperatura del chip (~34 °C en reposo).
- [x] heap_caps DMA y EXEC (EXEC da 0: el P4 no tiene montón ejecutable).
- [x] Los datos de la tarjeta (CID).

**Archivos**
- [x] `aos_hal_dir_scan` sobre FatFs: 3000 archivos listados de una pasada
      (el portal tardaba por un `stat` por archivo). Falta ver que las
      fechas lleguen en hora local (se vieron archivos de "1980-12-31"
      subidos antes de que llegue el SNTP).
- [x] Copiar con un buffer de 32 KB en PSRAM: 2,8 MB/s con `aos_hal_io_alloc`,
      el bounce buffer fijo y `read()` (MEMORY.md).

**Claude**
- [x] El primer login real, que confirma o corrige (2026-09-30, a la primera,
      con las direcciones por defecto):
  - [x] la dirección de canje del token;
  - [x] el redirect;
  - [x] los scopes;
  - [x] la forma de la respuesta de `/api/oauth/usage`: 5 h al 3 % y semana al
        82 %, con los mismos reinicios que muestra Claude Code.
  - [x] Que siga andando solo: a la noche del 2026-09-30, horas después del
        login y una docena de reinicios y OTAs, trajo los números a la
        primera. (El servicio arranca cuando algo lo consulta: la app, el
        widget o el portal.)
  - [ ] Que el ritmo aparezca después de 20 minutos de historial.

  Si algo falla, el portal muestra el error del servidor y las direcciones se
  cambian en las preferencias (`claude_*`) sin grabar firmware.

**Cámaras**
- [ ] tinyh264 a la resolución del timbre.
- [ ] El motor JPEG con los MJPEG.
- [ ] El escalado por CPU (si no alcanza, pasar al PPA).
- [ ] Cuatro cámaras a la vez con la RAM interna que queda.

**Macro pad (USB OTG en modo dispositivo)** (probado el 2026-09-30, todo seguido
por el Registro del portal sin cable serie)
- [x] Que la Mac enumere el dispositivo HS: "P4OS p4os" a 480 Mbit/s, a la
      primera (macOS pide aceptar el accesorio; configurado ~12 s después).
- [x] Los reports de teclado, multimedia y mouse: volumen 63 → 69 → 63
      medido desde la Mac, ⌘⇧3 dejó las capturas, Spotlight, pegar, puntero,
      clic con un toque y scroll a dos dedos para el lado correcto.
- [x] Que una tecla de 8 ms no se pierda: "hello world", Enter, dos borrados,
      exactos; ningún report fallido en el registro.
- [x] Los LEDs de vuelta: la Mac manda el report de salida (se ve en el
      registro).
- [x] Despertar a la Mac dormida: un botón del Macro pad la despierta (pide
      la clave, como debe ser). No llegó ningún "suspended": con la placa
      colgada del puerto, macOS no suspendió el bus, así que despertó con el
      report mismo y el remote wakeup no se usó. Falta verlo con un bus que
      sí se suspenda.
- [x] Alimentarse por el OTG: desenchufada la consola, la placa sigue sola
      con el OTG desde la Mac, sin reiniciarse.
- [x] **Modo disco** (2026-09-30, `docs/USB.md`): la microSD como pendrive
      desde Ajustes → USB; lectura 8,2 MB/s, escritura 3,3 MB/s, 64 MB
      íntegros, y expulsarla en la Mac la devuelve sola a la placa. Spotlight
      trababa la expulsión: ahora la placa deja `.metadata_never_index`.
- [x] **Mando, MIDI y red por el cable** (2026-09-30, `docs/USB.md`): el
      Macro pad suma las caras Mando y MIDI; la red da el portal en
      192.168.7.1 a 7,4 MB/s (4 veces el Wi-Fi). Tres arreglos de
      descriptor hasta que la Mac lo tomó (intervalo de NCM en HS, MIDI de
      un solo sentido, mando en su propia interfaz).
- [x] El modo del puerto se guarda (pref `usb_mode`): tras una OTA vuelve
      solo a Teclado y mouse a los 4,1 s y la Mac lo toma a los 4,5 s. El
      modo Disco no se guarda (el arranque lee la tarjeta).
- [x] **Host de pendrives** (2026-10-04, `docs/USB.md`): un Kingston
      DataTraveler 2.0 de 8 GB montado en `/usb` en los dos puertos. En
      21/23 (segundo controlador, Full Speed, pasado de GPIO26/27 a
      GPIO24/25 porque el 26 es la retroiluminación) montó con cables de
      70 cm, también con el pendrive puesto desde el arranque, y a la vez
      que el OTG era Teclado y mouse con la Mac (tomado en HS). En 25/27
      (alta velocidad) falló el reset del puerto con 70 cm y anduvo con
      menos de 15 cm: 7,4 MB/s de lectura en la placa; por Wi-Fi, ~450 KB/s
      en los dos.
- [x] Pasar el host de 21/23 a 25/27 con el pendrive montado reiniciaba la
      placa (assert en `hub_root_stop`): arreglado, probado.
- [x] Escribir en el pendrive: 4 MB por el portal, leídos de vuelta
      idénticos (SHA-256) y borrados (2026-10-04, con permiso del usuario).
- [x] **Los dos puertos a la vez** (2026-10-04, copia propia de la
      librería host en `components/usb`): teclado Chicony en 21/23 y el
      Kingston en 25/27, el pendrive a 7,5 MB/s con el teclado puesto, y los
      dos enchufados desde el arranque (el segundo espera la dirección 0).
- [x] Teclado por su descriptor HID: sus dos interfaces (teclado; teclas
      multimedia y de sistema) en la lista de dispositivos.
- [x] Escribir con el teclado físico en Notas (2026-10-04, el usuario).
- [ ] Distribución latinoamericana a fondo (ñ, tildes, @) y teclas
      multimedia (volumen).
- [x] Mouse Logitech (046d:c077, baja velocidad, 4 campos): puntero, clic y
      rueda andan (2026-10-04, el usuario).
- [x] Joystick genérico estilo SNES (081f:e401): cruceta como ejes X/Y
      (+-32767, diagonales incluidas) y d-pad; X=1, A=2, B=3, Y=4, L=5, R=6,
      Select=9, Start=10, leído en vivo por `/api/usb` (2026-10-04). Manda
      reportes todo el tiempo aunque se le pida SET_IDLE: sin problema.
- [ ] Teclado MIDI (`midi_last`, y MIDI thru con el OTG en modo teclado).
- [x] Adaptador CH340 (1a86:7523, "USB2.0-Serial") en 25/27 (2026-10-04):
      listado como `usb0`, abierto por el servicio serie a 115200 y 13
      bytes mandados (`/api/serial/send`).
- [x] Recepción por `usb0` con TX y RX puenteados (2026-10-04): eco
      idéntico (UTF-8 incluido) y los mismos bytes recibidos que enviados
      a 115200, 9600 y 921600 baudios.
- [x] **Arduino Leonardo** (2341:8036) con `tools/usb_test/p4os_leonardo`
      (2026-10-04): dispositivo compuesto, serie CDC-ACM y teclado HID a la
      vez, cada parte con su driver. Por `usb0` llegan sus líneas y el eco
      (`hola placa` -> `HOLA PLACA`); con `!` escribió "hola desde el
      Leonardo" en una nota (con el teclado de Notas abierto: sin él, Notas
      no toma teclas físicas).
- [x] CP2102 de una placa ESP32 (10c4:ea60, 2026-10-04): con los búferes
      en RAM interna, nada hasta apretar EN y después el registro de
      arranque limpio (ROM, bootloader IDF 5.5.5, ESPHome), 5172 bytes.
- [x] **FTDI FT232R** (0403:6001, 2026-10-04): eco con TX y RX puenteados
      a 9600, 115200 y 921600, cerrando y reabriendo entre cada una: todo
      vuelve. Antes se perdía el primer paquete tras cada reapertura (data
      toggle desfasado por el cierre del driver CDC-ACM): ahora el lado USB
      queda abierto hasta que se desenchufa. Al enchufarlo en caliente dos
      veces no enumeró: desde entonces la placa reintenta sola.
- [x] **Raspberry Pi Pico** (2026-10-04): en BOOTSEL se montó en `/usb`
      como disco; copiar MicroPython v1.29.0 (.uf2) ahí la programó (el
      portal contesta error porque el disco se va antes de cerrar el
      archivo: es la señal de que tomó). Volvió como CDC-ACM y su REPL
      contestó comandos mandados desde la placa.
- [x] **Webcam** Logitech C270 (046d:0825, sin nombre declarado) en 25/27
      (2026-10-04): MJPEG 640x480 (su máximo en MJPEG), 14-19 cuadros/s en
      la app Cámaras, decodificar 4-5 ms por cuadro, escalar a 720x540
      16-29 ms, sin cuadros descartados; "funciona muy bien" (el usuario).
      Antes hizo falta: descriptores de configuración de hasta 4 KB (la
      librería traía 256) y sacar dos tablas de la pila de la tarea del
      driver UVC (se desbordaba al llegar la cámara: reinicio).
- [x] Pendrive con los búferes DMA del host en PSRAM: 6,1-6,4 MB/s (en
      RAM interna, 7,4). **Pero se volvió a RAM interna**: con PSRAM un
      CP2102 leía 400 KB/s de basura repetida (imposible por el cable) y
      cerrar el puerto en medio abortaba en el driver CDC-ACM (reinicio).
- [x] **Hub** de 7 puertos (dos chips de 4 encadenados) en 21/23 con el
      joystick (2026-10-04): entra en cualquier puerto y se lee. Visto: (a)
      una vez el hub contestó STALL a GET_PORT_STATUS justo tras encender
      sus puertos y quedó descartado hasta desenchufarlo (no se repitió en
      ~6 conexiones más); (b) 2 de 4 veces, al resetear el puerto del
      joystick (baja velocidad) se cayó el hub entero y volvió solo.
      Probablemente los 5 V que caen por los cables. Queda así: anda con
      hub, se recomienda cablear directo (el usuario no lo va a usar con
      hub; decisión del 2026-10-04).
- [ ] Varios pendrives detrás del hub (`/usb2`, `/usb3`).
- [x] **Placa de audio USB** GeneralPlus (1b3f:2008, 2026-10-04): salida 48
      kHz estéreo, micrófono 48 kHz mono y teclas de volumen (HID). La radio
      (MP3 44,1 kHz remuestreado a 48) sonó por auriculares y el parlante de
      la placa se calló; "suena limpio" (el usuario), 4 bloques de 10 ms
      perdidos en 12 s. Su micrófono en la Grabadora: 335 376 muestras en
      6,99 s (48 kHz, sin pérdidas), ruido de fondo media 74 pico 2944 (sin
      micrófono enchufado). Falta: un micrófono de verdad.
- [ ] Visto de paso: `E i2s_common: i2s_channel_disable … not enabled` al
      reabrir el códec para el sonido de las teclas; inofensivo, a limpiar.

**Módulos**
- [ ] Abrir y cerrar el bus I2C en cada vuelta.
- [ ] Más de 8 dispositivos en un puerto.
- [ ] Que los chips reales se identifiquen como supone el código (SHT4x,
      INA219, AHT20).
- [ ] Precisión contra un instrumento de referencia.

**Lienzo retro**
- [x] El escalado del PPA dejaba mal la primera fila de cada bloque: Atrapa
      (Lua) dejaba una raya por cada estrella que caía (2026-10-05). Desde
      ed96d74 escala la CPU por omisión (`retro_hw=1` en `/api/settings`
      vuelve al PPA). Medido por CPU: Atrapa 49,8 fps (escalar 0,25 ms),
      2043 a pantalla completa 29 fps (escalar 4,2-4,9 ms, refresco 15 ms);
      los dos se ven bien. `/api/sysmon` muestra el lienzo en `lvgl.retro`.
- [ ] Que el PPA acepte los buffers de LVGL.
- [ ] Que no haya costuras entre franjas.
- [ ] Jugar con dos dedos.

## Pendientes anotados en la placa

- [x] **Traducciones al inglés** (visto en la placa en inglés, 2026-09-29;
      hechas el 2026-09-30 en 1b7c445, en/de completas; queda sólo lo del
      teclado de LVGL claro sobre la hoja oscura, en "Chicos" arriba):
  varios nombres de apps en el inicio siguen en castellano (Reloj, Red,
  Banco, Módulos, entre otros) y dentro de Ajustes hay textos sin traducir.
  Revisar que los nombres pasen por `N_()`/`_()` y que estén en el paquete de
  idioma (`tools/gen_lang.py`); se hace junto con la próxima compilación del
  firmware.
  Nuevos de las carpetas (2026-09-29): "Editar", "Nombre de la carpeta",
  "Carpeta vacía"; y el teclado de LVGL sale claro sobre la hoja oscura.
- [x] **Servicio del lienzo retro** (hecho en 15ff279), pedido por los juegos
      pasados a pantalla completa (2026-09-29): 2043 y Claude Jump dibujan y
      leen sus propios botones porque el servicio no alcanza.
  - En modo `OVERLAY`, un dedo sobre un botón también se informa como dedo del
    lienzo (se elige el primero que tocó): excluir de `aos_retro_touch()` y
    `aos_retro_tap()` los dedos que caen sobre un control.
  - Un flag `AOS_RETRO_LR_SPLIT`: IZQUIERDA abajo a la izquierda y DERECHA
    abajo a la derecha (a los lados de la píldora de pausa; en horizontal,
    uno por columna lateral).
  - A/B abajo a la derecha en vertical (hoy se centran si no hay control a la
    izquierda).
  - `docs/RETRO.md` sigue diciendo que los juegos fuerzan vertical y describe
    ARKANOS con slider y botón A: ya no es así.
- [x] **Claudito** (hecho: por filas que cambian) redibujaba casi todo el lienzo en cada cuadro (~9,6 pantallas
      por segundo, 15 % de un núcleo): pasar a rectángulos sucios.
- [ ] **2043** en la placa: ~24 fps (apunta a 30); sigue en "Del sistema"
      arriba.
- [x] **Pantalla de arranque de P4OS** (pedido del usuario, 2026-09-29): al
      arrancar se ven aparecer los íconos de las apps de a uno mientras se lee
      la tarjeta. Una animación de arranque (~5 s) que cubra hasta que el
      inicio esté completo. Se hace cuando haya más apps portadas, para medir
      cuánto tarda de verdad todo en estar cargado. Punto de partida medido:
      19 apps de la tarjeta registradas en 3,3 s (casi 2 s de eso es dar de
      alta los íconos, `aos_dynapp` "register"), más ~5 s hasta que el Wi-Fi
      conecta. Que termine cuando el escaneo termine, no por reloj.
      Hecho el 2026-09-30 (04c5309): la minimalista que eligió el usuario
      (P4OS en blanco sobre negro y una barra fina con el avance real del
      escaneo), y el inicio se arma una sola vez al final del escaneo: con
      40 apps el escaneo bajó de 8,3 s a 1,5 s y el sistema queda listo a
      los 4,2 s. Visto: sin hora real hasta que llega el SNTP ("--:--",
      "WED 31 DEC"): la placa no tiene chip RTC (HARDWARE.md), sólo el
      RTC interno del P4; queda la prueba 17 (si guarda la hora con una pila
      en H3).
- [x] **Pedidos al sistema de las apps portadas** (2026-09-29; hechos salvo
      el simulador, que sigue en "Del sistema" arriba):
  - `AOS_APP_FLAG_KEEP_AWAKE` sólo despierta la pantalla al abrir la app
    (`aos_ui.c`): que la mantenga despierta mientras la app esté al frente
    (Doom lo esquiva llamando a `aos_hal_activity()` cada 5 s).
  - Las apps que vuelcan directo al framebuffer no saben cuándo hay un panel,
    banner o el selector de apps encima, y los pisan: un
    `aos_ui_overlay_active()` o un aviso de pausa/reanudar.
  - Doom en la placa: 34 fps, volcado 7,1 ms. En una captura del framebuffer
    se vio un corte vertical entre dos cuadros (el PPA escribiendo mientras
    se lee): mirar si se nota como desgarro en el vidrio.
  - `aos_retro_begin()` borra `s.gen` con el `memset` (visto por Lua).
  - Simulador: `aos_hal_sim_link_tick()` no se llama nunca (el enlace del sim
    no entrega tramas) y `LUAI_MAXCCALLS=100` para `aos_lua` como en la placa.
  - Página `/lua` en el portal (sólo `components/aos_portal/web/app.js`, con
    la API de archivos que ya existe) y `/pixel`.
- [x] **Juegos acostados, techo del PPA** (medido con Monster Hop, 2026-09-29):
  parado va a 20 fps y acostado a 15,4, porque girar el cuadro de 1280x720
  con el PPA tarda 62 ms. Girarlo con la CPU cuesta más (~42 ms por núcleo,
  PSRAM). Hecho el 2026-09-29: tres framebuffers del panel con
  `aos_hal_display_back/flip` (parado 20 -> 22 fps, sin cortes; acostado
  sigue en ~15,5 porque el PPA y el dibujo se pelean la PSRAM). Falta: que
  una app consiga dos bloques de 20 KB de RAM interna para girar franjas
  con el PPA (32 ms el cuadro, medido); con franjas de 4 filas el PPA se
  cuelga.
