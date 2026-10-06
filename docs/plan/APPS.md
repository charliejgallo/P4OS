# P4OS — Relevamiento de apps: qué migra de AmoledOS, qué no, y qué es nuevo

Base: AmoledOS v0.9.1 (`main` f422a7d). Tiene 20 apps internas y 40 dinámicas.

## Cómo se migra (cuatro recetas)

Al ser la densidad de píxeles casi la misma (294 contra 322 ppi), **no se agranda
nada**: lo que cambia es cuánto entra.

| Receta | Qué implica | Costo |
|---|---|---|
| **R — Re-maquetar** | La app usa objetos de LVGL. Se sacan los números fijos pensados para 368×448, se pasa a flex/grid y a `AOS_SCREEN_W/H` en tiempo de ejecución, y se aprovecha el espacio: más filas, dos columnas en horizontal. | Bajo-medio (hay ~450 posiciones literales repartidas en las apps dinámicas) |
| **C — Lienzo retro** | Los juegos que ya dibujan en 184×224 y escalan ×2 a mano pasan a un **servicio del SO**: la app dibuja su lienzo chico y el PPA lo escala **×3 por hardware** (552×672, centrado) o a una variante más ancha (240×426 ×3 = 720×1278, pantalla completa, si el juego tolera ver más mundo). | Bajo: se borra el escalado a mano y se gana velocidad |
| **A — Assets nuevos** | Juegos con packs pre-renderizados a 368×448 desde Blender (golf, mila, monsterhop, turbo). Los pipelines están en cada `apps/*/tools`: **se re-renderizan** a la resolución nueva. Los packs crecen ~4× (Mila: 6,6 → ~26 MB en la SD). | Medio-alto, pero es tiempo de máquina más que de código |
| **W — Worker + framebuffer** | Apps que blittean directo al panel (`aos_hal_display_blit`). En la P4 el blit **escribe al framebuffer DPI** (con PPA para escalar o rotar), más simple y rápido que el QSPI por franjas del reloj. | Medio: cambia el HAL, casi nada la app |

Todas las `.so` se recompilan para RISC-V (la tabla de símbolos se regenera; el
P4 tiene FPU de simple precisión y desaparecen los `__addsf3` de Xtensa).

## Apps internas (firmware)

| App | Decisión | Receta / notas |
|---|---|---|
| Ajustes | ✅ **Reescribir** | Lo más grande del reloj (3,7k líneas) y lo más ligado a él (PMU, calibración, esferas). Se reescribe con estilo iOS: lista con secciones y subpáginas. Se reutilizan WiFi, AP/QR, idioma, No molestar, USB, respaldo. |
| Música | ✅ Migrar | R. Vista de lista + reproductor grande con tapa; en horizontal, dos paneles. |
| Fotos | ✅ Migrar | R. Galería en grilla + visor con pinch; JPEG por hardware. |
| Relojes: Mundial, Alarma, Temporizador, Cronómetro, Pomodoro | ✅ Migrar, **juntas** | R. Se unen en una app "Reloj" con pestañas abajo, como iOS. |
| Calendario | ✅ Migrar | R. Mes completo; a futuro, eventos del calendario de HA. |
| Calculadora | ✅ Migrar | R. La Braun en vertical; en horizontal, científica. |
| Conversor | ✅ Migrar | R. Se suma una pestaña de **electrónica** (ver Calculadoras de electrónica). |
| Life | ✅ Migrar | R. Grilla mucho más grande (180×320 celdas a 4 px). |
| Pato goma | ✅ Hecha (2026-10-06) | `.so` de tarjeta `apps/pato/` (`aos.pato`), con ícono, página del portal y **editor de flujos en la placa** (nuevo: el reloj sólo elegía y corría). El usuario revirtió la decisión del 2026-09-30. Queda la copia vieja sin portar en `components/aos_apps/_pending/` (puede borrarse). |
| Control PC | ✅ Hecho dentro del Macro pad | No es una app aparte: el Macro pad tiene todo lo que hacía (pad multimedia, trackpad con zoom por pellizco, mando, MIDI). Lo que dependía del IMU (air mouse, mando por inclinación) no existe en esta placa. |
| Enlace | ⏸️ Depende de la Fase 2 | Si ESP-NOW anda por el C6, se migra (apareo por botón o QR, no por choque). Si no, pasa a enlace por UDP en la LAN. |
| Actividad (pasos) | ❌ No migra | Sin IMU. |
| Nivel | ❌ No migra | Sin IMU. |
| Linterna | ❌ No migra | No tiene sentido en un aparato de banco (el Centro de control tendrá "pantalla blanca" si hace falta). |
| Notificaciones del iPhone (ANCS) | ⏸️ Más adelante | Requiere BLE por el C6 (prueba 11). Poco valor en un aparato fijo; se reemplaza por notificaciones de HA. |
| Remoto de música del iPhone (AMS) | ⏸️ Más adelante | Igual que ANCS. |
| Esferas (7) | ♻️ Pasan a ser protectores de pantalla | Se dibujan en grande; se quitan batería y pasos. |

## Apps dinámicas (SD)

### Ola 1 — Re-maquetar, sin dependencias raras (validan la API nueva)

| App | Receta | Notas |
|---|---|---|
| hello_app | R | Plantilla para P4OS; primera `.so` que corre en RISC-V (prueba 19). |
| cotiz | R | Tabla completa, gráfico histórico al lado en horizontal. |
| clima | R | Pronóstico de 7 días visible sin desplazar; va también como widget. |
| escaner | R | Pasa a ser parte de "Herramientas de red" (lista de hosts, puertos, ping). |
| radio | R | Dial grande, lista de 33 radios al lado. |
| recorder | R | La que más posiciones literales tiene (35). Con el ES7210: estéreo. |
| afinador | R | Con dos micrófonos; el sonómetro va como herramienta de banco. |
| sensores | R | Se funde con la app de Home Assistant. |
| remoto | ❌ reemplazada | Sus gestos de muñeca no aplican. La reemplaza **Home Assistant** (nueva). La configuración de páginas se importa. |
| dados | R | Tocar para tirar (no hay sacudida). |
| simon, flappy, gemas, atasco, buscaminas, blackjack, truco | R | Juegos de tablero/cartas; truco a dos por enlace si la Fase 2 lo permite. |
| pixel | R | Lienzo mucho más grande (32×32 y 64×64). |
| lua | C + R | El API de píxeles pasa por el lienzo retro; la página `/lua` tal cual. |

### Ola 2 — Lienzo retro (juegos a 184×224)

| App | Receta | Notas |
|---|---|---|
| arkanos | C | Paleta con el dedo (la inclinación desaparece). BOOT no está a mano: botón en pantalla. |
| burbujas | C | |
| cjump | C | Sin IMU: tocar izquierda/derecha. |
| g2043 | C | Disparo con botón en pantalla. |
| topos | C | |
| claudito | C | 92×112 ×6 = 552×672. |
| chatarra | C | El más grande (16,7k líneas). El enlace de las cabinas depende de la Fase 2. |

### Ola 3 — Worker/framebuffer y assets pesados

| App | Receta | Notas |
|---|---|---|
| camaras | W | La que más gana: 720×1280 o 1280×720 a pantalla completa, varias cámaras en mosaico. Frigate. **Hecha en el simulador (2026-09-28)**: mosaico con un hilo por cámara, visor, fotos a `photos/camaras`, pestaña Frigate; `cameras.txt` en la tarjeta. Falta medir en la placa ([CAMERAS.md](../CAMERAS.md)). |
| video | W | Sin la restricción de 368×448: MJPEG/AVI a la resolución de la pantalla, decodificado por hardware. |
| visor3d | W | |
| mapas | W | Mucho más mapa visible; revisar el costo por píxel del render (en el reloj se midió con `maps/bench.txt`). |
| doom | W | **Siempre horizontal**: 320×200 ×3 = 960×600 centrado, controles táctiles a los costados. |
| golf | A + W | Re-render de `golf.pak`. |
| mila | A + W | Re-render de `mila.pak`. |
| monsterhop | A + W | Re-render del pak (hoy 13 MB en partes). |
| turbo | A + W | Re-render de autos y escenarios; volante táctil (la inclinación era opcional). |
| neon | R | El campo es 368×448 fijo: se agranda el campo, no la víbora. |

### Ola 4 — Dependen del enlace (Fase 2, pruebas 10 y 11)

| App | Notas |
|---|---|
| pong | Requiere enlace. Si ESP-NOW anda: P4 contra reloj. |
| walkie | Requiere enlace + audio full dúplex. **P4 ↔ reloj** sería lindo de verdad. |
| radar | Requiere FTM por el C6: probablemente no entra (se cuenta en "No migran"). |

### No migran

| App | Por qué |
|---|---|
| laberinto | Sólo se juega inclinando: sin IMU no tiene sentido. |
| radar | Salvo que el C6 exponga FTM por hosted (poco probable). |
| remoto | Reemplazada por Home Assistant. |

## Apps nuevas (el corazón de P4OS: asistente de electrónica y domótica)

Ordenadas por prioridad aproximada; las primeras son las que definen el aparato.

| App | Qué hace | Depende de |
|---|---|---|
| **Home Assistant** | Paneles con entidades (luces, climas, persianas, escenas, sensores con gráfico, cámaras), vía **WebSocket API** con token de larga duración: estado en vivo, llamadas a servicios. Widgets en el inicio y escenas en el Centro de control. Importa las páginas de `remoto`. | WiFi |
| **Terminal serie** | Monitor de uno o varios puertos: `uart.a/b` del header, USB CDC (con hub alimentado). Baudios, colores ANSI, marcas de tiempo, filtros, resaltado, hex, **disparadores** que notifican, grabación a la SD. Corre como **servicio**: sigue capturando con otra app abierta. | Módulos, SD |
| **Programador** | Graba ESP32/ESP8266/STM32 por `uart.b` con EN/BOOT (esp-serial-flasher), con el firmware subido por el portal o bajado de una URL (releases de GitHub). Y **puente RFC2217**: `esptool --port rfc2217://p4os.local:4000` graba desde la Mac a través de la P4. | Módulos, WiFi |
| **Herramientas de bus** | Scanner I2C, lector/escritor de registros, panel de GPIO en vivo (el header dibujado), PWM, ADC como voltímetro y registrador. | Módulos |
| **Modbus** | Maestro RTU (por RS485) y TCP: leer/escribir registros, tablas guardadas, sondeo. Perfiles para el **gateway DOMCOM** y la **Riden RD6012** (que habla Modbus RTU por TTL). | RS485 o WiFi |
| **Banco** | Paneles para lo que ya tenemos: Riden RD6012 (tensión/corriente/salida, gráfico), osciloscopio Rigol por SCPI en la LAN, generador UTG932E (USBTMC; necesita host USB alimentado). Reusa lo aprendido en `riden-psu/`, `rigol-scope/` y `UTG900E/`. | RS485/WiFi/USB |
| **Calculadoras de electrónica** | Colores de resistencias, códigos SMD, ley de Ohm, divisor, resistencia de LED, 555, RC, conversión de unidades de ingeniería, dB. | — |
| **MQTT** | Explorador de tópicos, publicar, gráfico de un tópico. | WiFi |
| **Herramientas de red** | Ping, escáner de hosts/puertos (reusa `escaner`), analizador WiFi, mDNS. | WiFi |
| **Macro pad** | Botonera configurable (tipo Stream Deck): cada botón manda un atajo por USB HID, llama un servicio de HA o publica MQTT. Incluye trackpad y teclado (reemplaza a Control PC). | USB OTG, WiFi |
| **Monitor del sistema** | CPU por núcleo, tareas, memoria interna/PSRAM, temperatura del chip, red, SD. | — |
| **Consumo de Claude** | Widget y app con los sensores de `ha-claude-usage` que ya están en HA. | HA |
| **Pantalla USB** | La P4 como monitor secundario de la Mac por USB (Waveshare trae un ejemplo, `12_usb_extend_screen`). | USB OTG |
| **Asistente de voz** | Satélite de **HA Assist**: los 2 micrófonos del ES7210 con cancelación de eco por el canal de referencia. | Audio full dúplex, HA |
| **Cámara** | Sólo en el modelo "-C": vista previa, fotos, video H.264 por hardware, cámara RTSP para HA/Frigate. | Cámara CSI |
| **Analizador lógico** | 8 canales por PARLIO, decodificación UART/I2C/SPI. | Módulos (Fase 8) |

## Conteo

- Internas: **13 migran** (algunas unidas), 1 se rediseña, 3 no migran, 3 quedan
  para más adelante, y las esferas se reaprovechan.
- Dinámicas: **37 migran** en 4 olas (pong y walkie atadas al enlace), **2 no
  migran** (laberinto; remoto, reemplazada por Home Assistant), y radar casi
  seguro que tampoco.
- Nuevas: **16**, de las cuales las 6 primeras son el núcleo del aparato.
