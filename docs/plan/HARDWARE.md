# P4OS — Hardware: Waveshare ESP32-P4-WIFI6-Touch-LCD-5

Lo que sabemos **antes** de tener la placa, sacado de la documentación oficial, el
repo de Waveshare, el BSP 1.0.4 y la lectura directa del esquemático. Todo lo que
dice **(esquemático)** es nuestra lectura del dibujo y hay que confirmarlo en la
Fase 2 (pruebas de hardware). La columna "medido" de la tabla de pruebas del
final se llena cuando llegue la placa.

Referencias guardadas en `docs/hw/`:
- `ESP32-P4-WIFI6-Touch-LCD-5-Schematic.pdf` y `schematic-crops/` (recortes por bloque)
- `ESP32-P4-WIFI6-Touch-LCD-5-dimensions-20260408.pdf` (126,9 × 70,7 mm, 4× M2.5)
- `esp32-p4_datasheet_en.pdf`
- `waveshare/` — docs del repo (IO, HARDWARE, COMPONENTS, P4_C6_HOSTED_WIFI, CI),
  el BSP 1.0.4, los 12 ejemplos ESP-IDF y el firmware de fábrica (para volver atrás)

Fuentes online: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-5 ·
https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-5 ·
https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_5

## Resumen

| Bloque | Qué hay | Notas para P4OS |
|---|---|---|
| SoC | ESP32-P4NRW32, 2× RISC-V HP a 360/400 MHz + 1 LP, FPU simple precisión | Revisión: los ejemplos asumen **v3.x** (PSRAM 250 MHz); hay perfil rev1.x. Un usuario reporta v3.2. **Ver con `esptool chip_id` apenas llegue.** |
| Memoria | 32 MB PSRAM en el chip, 768 KB SRAM HP, flash externa **32 MB** (GD25Q256) | El doble de flash que el reloj. La RAM interna deja de ser el cuello de botella que fue en el S3. |
| WiFi / BLE | ESP32-C6-MINI-1 (4 MB) por **SDIO 4 bits**, WiFi 6 sólo 2,4 GHz, BLE 5 | Con `esp_hosted` 1.4.* + `esp_wifi_remote` 0.14.* en IDF 5.5. Firmware del C6 cerrado, versión desconocida. **Riesgo principal** (ver abajo). |
| Pantalla | 5" IPS **720×1280 vertical nativo**, HX8394, MIPI-DSI 2 carriles a 700 Mbps | DPI con framebuffers en PSRAM (el BSP usa 3). ~55 Hz con los tiempos del BSP. ~294 ppi. |
| Retroiluminación | Boost AP3032; **GPIO26** PWM (LEDC 5 kHz), **GPIO33** enable | GPIO33 tiene pull-up de 100K a 5 V: no usarlo para otra cosa. |
| Táctil | **GT911**, 5 puntos, I2C 0x5D ó 0x14, RST GPIO23 | INT en GPIO2 **sólo si R108 está puesta** (figura "NC/0R"): probablemente hay que leerlo por polling. |
| Audio | **ES8311** (DAC/parlante) + **ES7210** (ADC de 4 canales, 2 micrófonos + referencia de eco) + amplificador NS4150B | Mic y parlante son chips distintos: **full dúplex** de verdad, a diferencia del reloj. I2S: MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 11, PA 53. |
| Cámara | Conector MIPI-CSI de 2 carriles (OV5647 en el modelo "-C") | Codificador H.264 por hardware hasta 1080p30. ¿Qué modelo compraste? |
| microSD | SDMMC de 4 bits en los pines IOMUX: CLK 43, CMD 44, D0-D3 39-42; alimentación por GPIO45 | Usa el LDO VO4 del P4 para los pull-ups. No tiene detección de tarjeta. |
| USB | **H1 "UART"**: CH343P → UART0 (GPIO37/38), auto-reset. **H2 "OTG"**: USB 2.0 High-Speed del P4 | **H2 no entrega 5 V** (esquemático): para usar el P4 como host USB hace falta un hub alimentado. El USB-Serial-JTAG (GPIO24/25) no llega a ningún conector, sólo al header. |
| RTC | No hay chip RTC. Hay cristal de 32 kHz (GPIO0/1) y la entrada H3 para una pila recargable al VBAT del P4 | Probar si el RTC interno del P4 mantiene la hora con la placa apagada. |
| Batería | Conector MX1.25 para Li-ion, cargador ETA6098, elevador a 5 V; **tensión por GPIO20** (divisor 1/3, ADC1 canal 4) | Sin PMU: sólo tensión, sin corriente ni coulomb counter. Si se conecta batería, se muestra el porcentaje estimado. |
| IMU | **No hay** | La orientación se elige a mano. |
| Botones | BOOT (GPIO35), RESET (EN) y POWER (Key3 con un controlador de encendido: corto enciende, 2 s apaga) | El botón POWER no llega a ningún GPIO: el SO no se entera de que apagan. BOOT (desde el 2026-09-30, `aos_hal_p4.c` "The BOOT button", `aos_ui.c`): primero es de la app abierta (`button()`); si no lo usa, un toque va al inicio, uno largo (≥ 0,8 s) guarda la pantalla en `/sdcard/photos/Capturas` (un álbum de Fotos), y con la pantalla apagada sólo la enciende. Apretado mientras se ve la pantalla de arranque: **modo seguro** (sin las apps de la tarjeta, sin las preferencias de ajuste, USB quieto). Apretado en el reset manda al chip al modo de descarga de la ROM: por eso no sirve "mantener al encender". |
| LEDs | Sólo el de alimentación | |
| Ethernet | No hay | |

## Pines usados adentro de la placa (no tocar)

| GPIO | Uso |
|---|---|
| 0, 1 | Cristal de 32 kHz |
| 6 | Línea al C6 (IO2, handshake/data-ready de hosted) |
| 7, 8 | **I2C de la placa** (táctil, ES8311, ES7210, cámara): **también sale al header** |
| 9-13 | I2S del audio |
| 14-19 | SDIO al C6 |
| 20 | ADC de la batería |
| 23 | Reset del táctil |
| 26 | PWM de la retroiluminación |
| 27 | Reset del LCD (con inversor NPN) |
| 33 | Enable de la retroiluminación |
| 35 | BOOT (strapping), también sale al header |
| 36 | Strapping (log de la ROM / modo de arranque), pull-up 10K |
| 37, 38 | UART0 al CH343P: consola y grabación. **Salen al header pero están ocupados.** |
| 39-44 | microSD |
| 45 | Alimentación de la microSD |
| 53 | Enable del amplificador |
| 54 | Reset del C6 |

El header de 40 pines y su plan de uso están en [EXPANSION.md](EXPANSION.md).

## Riesgos de hardware conocidos antes de empezar

1. **El C6 y esp_hosted.** Hay un issue abierto (#13 del repo de Waveshare): con
   Arduino / esp_hosted 2.12 el C6 de fábrica no contesta RPCs, reporta versión
   0.0.0 y el WiFi anda inestable. La combinación estable es la de los ejemplos:
   **IDF 5.x + esp_hosted 1.4.* + esp_wifi_remote 0.14.***. Si igual falla,
   el plan B es grabar el firmware esclavo de esp_hosted 1.4 en el C6: por las
   islas J7 (TX, RX, IO9, GND; hace falta un adaptador USB-UART de 3,3 V) o
   por OTA desde el P4.
2. **ESP-NOW, FTM y BLE a través del C6.** El enlace con los relojes
   (`aos_link`) usa `esp_now` directo, y en la P4 eso pasa por RPC al C6.
   Hay que ver qué soporta esp_hosted 1.4. El plan B es que el enlace vaya por
   UDP en la LAN: el simulador ya lo hace así.
3. **Revisión del chip.** Un binario rev3 no arranca en rev1.x y al revés:
   `sdkconfig.defaults` con dos perfiles y un directorio de build por perfil.
4. **GPIO46-48 del header** están en el dominio VDD_IO_5, que se alimenta del
   LDO VO4 (50 mA máximo, el mismo de la SD). Medir la tensión en esos pines
   antes de conectarles nada.
5. **El 5 V del header** sale antes del interruptor de encendido: los módulos
   externos probablemente siguen alimentados con la placa "apagada".
6. **Táctil sin interrupción** si R108 no está puesta: polling del GT911.
7. **Sin 5 V en el USB OTG**: el modo host necesita alimentación externa.

## Pruebas de la Fase 2 (banco `p4bench`)

Cada prueba tiene un criterio y un lugar para anotar lo medido. El firmware
`p4bench` se deja compilado **antes** de que llegue la placa, para grabarlo
apenas se enchufe. Las decisiones marcadas con 🔒 congelan algo que después
cuesta cambiar (ABI de LVGL, perfil de chip, mapa de pines).

| # | Prueba | Cómo | Criterio / qué decide | Medido |
|---|---|---|---|---|
| 0 | Identidad | `esptool chip_id`, `flash_id`; log de arranque | Revisión del chip 🔒 perfil de build; flash 32 MB; PSRAM 32 MB | |
| 1 | Firmware de fábrica | Arrancarlo tal cual y anotar qué hace; respaldar la flash entera (`esptool read_flash`) | Tener cómo volver atrás | |
| 2 | PSRAM | memcpy/memset PSRAM↔PSRAM, SRAM↔PSRAM, con y sin 2D-DMA | Ancho de banda real (define cuánto cuesta un cuadro completo) | |
| 3 | Pantalla cruda | Barras de color, llenado del framebuffer completo por CPU, por DMA2D y por PPA; alternar 2 y 3 framebuffers | Cuadros por segundo a pantalla completa; ¿se ve el tearing? | |
| 4 | Retroiluminación | Barrido de PWM 0-100 %, apagado por GPIO33 | Curva de brillo útil, mínimo visible, consumo | |
| 5 | LVGL | `lv_demo_benchmark` y `lv_demo_widgets` en vertical y horizontal; 1 y 2 draw units; `LV_USE_PPA` sí/no; buffer parcial y directo | 🔒 **config de LVGL (ABI de las apps)** y método de rotación | |
| 6 | Rotación | Horizontal por rotación de LVGL, por PPA en el flush y por DMA2D | fps en horizontal ≥ 80 % de vertical | |
| 7 | Táctil | Dirección I2C, 5 dedos, frecuencia de reporte, ¿hay INT en GPIO2?, cobertura de bordes con vista cruda | Polling o INT; ¿hace falta calibrar bordes? (en el reloj la calibración **fabricaba** franjas muertas) | |
| 8 | microSD | Lectura/escritura secuencial y de archivos chicos a 20/40 MHz, ¿SDR50? | MB/s; si hace falta el parche de escritura alineada | |
| 9 | WiFi | Versión del firmware del C6, conexión, iperf, reconexión tras cortar el AP, SNTP, mDNS, TLS | Estable 1 h sin caídas; si no, plan B del C6 | |
| 10 | ESP-NOW | ~~Beacon y ping con un reloj AmoledOS~~ | **Resuelto sin placa (2026-09-27): esp_hosted 1.4.7 no trae RPCs de ESP-NOW; `esp_now_init` ni siquiera enlaza.** El enlace con los relojes va por UDP en la LAN. Revisar si una versión nueva de hosted, o un firmware propio del C6, lo habilita. | no disponible |
| 11 | BLE | Scan con NimBLE sobre hosted (HCI por SDIO) | Si anda: mandos BLE y ANCS a futuro | |
| 12 | Audio | Tono y WAV por el ES8311, volumen, amplificador; grabación de 2 canales por el ES7210; **las dos cosas a la vez** | Full dúplex (walkie, asistente de voz, AEC) | |
| 13 | JPEG / H.264 / PPA | Decodificar JPEG 720×1280 por hardware; H.264 704×576 y 1080p por software (esp_h264); escalar y rotar con PPA | fps para Cámaras, Video y fondos de pantalla | |
| 14 | USB OTG | Modo dispositivo HS (CDC, HID, MSC, NCM); modo host con hub alimentado (CDC-ACM, CH34x, CP210x, FTDI, USBTMC) | Qué modos entran en P4OS | |
| 15 | Header | Lazo por pares de pines con jumpers, UART en lazo hasta 3 Mbaud, scan I2C del bus de la placa, **tensión de GPIO46-48**, qué hacen los pines de strapping | 🔒 **perfil de pines por defecto** | |
| 16 | 5 V y 3V3 del header | ¿El 5 V sigue vivo con la placa apagada? Caída con 500 mA | Documentar para los módulos | |
| 17 | RTC | Hora del RTC interno tras apagar con el botón, y tras desenchufar (con y sin pila en H3) | Si hace falta RTC externo por I2C | |
| 18 | Batería | Si hay una Li-ion a mano: ADC de GPIO20 contra multímetro | Mostrar batería o esconderla | |
| 19 | Cargador ELF | `hello.so` desde la SD, ejecutado desde PSRAM; una `.so` con float y una con LVGL | Reubicaciones RISC-V completas (el cargador 1.3.3 hay que actualizarlo) | |
| 20 | Consumo y temperatura | Medidor USB en reposo, brillo máximo, WiFi activo, CPU al 100 %; sensor de temperatura del chip tras 30 min | Presupuesto de energía; ¿hace falta bajar la frecuencia en reposo? | |
| 21 | Cámara (si es el modelo "-C") | Vista previa OV5647 por CSI + ISP | Si entra una app de cámara | |
