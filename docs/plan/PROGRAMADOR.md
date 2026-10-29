# Programador universal: el asistente de taller

Plan del 2026-10-27. La placa tiene que grabar, leer y borrar los
microcontroladores comunes del mercado **sin la computadora**, y cuando la
computadora está, prestarle sus pines como sonda. Este archivo dice qué se
hace, en qué orden y cómo se prueba; cada fase, al terminar, pasa a
`docs/MODULES.md` (lo que quedó) y a `PRUEBAS-PLACA.md` (lo medido).

## Lo que hay hoy (0.22.2)

- **ESP32 y ESP8266 por serie** (`components/aos_flasher`, sobre
  esp-serial-flasher): desde `uart.b` del header con EN y BOOT, o por USB
  (CP210x, CH34x, FTDI, CDC-ACM, la USB-Serial-JTAG propia de los C3/C6/S3/H2).
  Graba carpetas de ESP-IDF (`flasher_args.json`) y `.bin` sueltos, verifica
  por MD5. **No lee** (no hay volcado) ni borra.
- **Arduino AVR por su bootloader** (F3, sin publicar): STK500 v1/v2 y
  AVR109, desde un `.hex`; leer y borrar el programa.
- **Pico (RP2040) en BOOTSEL**: es un disco USB; copiar un `.uf2` a `/usb` lo
  graba (`docs/USB.md`). Fuera del Programador.
- **Terminal serie**: monitor de los UART del header y los USB.
- **SWD** (F2, sin publicar): nRF52, STM32F0/F1 y RP2040; grabar,
  verificar, borrar entero y la consola RTT.
- **Nada de STM32 por serie.**

## Familias y caminos

| Familia | Ejemplos | Camino | Grabar | Leer | Fase |
|---|---|---|---|---|---|
| ESP32 / ESP8266 | DevKit, NodeMCU, módulos | ROM serie (hay) | hay | F1 | F1 |
| Cortex-M por SWD | RP2040, nRF52 (la Ekobadge), STM32, SAMD, GD32 | SWD con 2-3 cables | hay (sim): nRF52, STM32F0/F1, RP2040 | sólo verificar | F2 |
| AVR con bootloader serie | Uno, Nano, Pro Mini, Mega | STK500 v1 (optiboot) y v2 (Mega) | hay (sim) | hay (sim) | F3 |
| AVR con USB propio | Leonardo, Micro, Pro Micro | AVR109 (Caterina), toque a 1200 baudios | hay (sim) | hay (sim) | F3 |
| STM32 sin sonda | Blue Pill, Black Pill | bootloader serie de ST (AN3155), BOOT0 y NRST con las líneas EN/BOOT | F4 | F4 | F4 |
| Desde la Mac, serie | esptool, avrdude, stm32flash | puente RFC2217 / TCP crudo al puerto | (la Mac) | (la Mac) | F4 |
| Desde la Mac, SWD | OpenOCD, pyOCD, probe-rs | la placa como sonda CMSIS-DAP v2 por el USB OTG | (la Mac) | (la Mac) | F5 |
| AVR sin bootloader | ATmega/ATtiny sueltos, grabar el bootloader de un Uno | ISP por `spi.a` + RESET; fusibles | F6 | F6 | F6 |
| AVR nuevos | ATtiny 0/1/2, AVR Dx, Nano Every | UPDI por un UART (una resistencia) | F6 | F6 | F6 |
| RP2040 / RP2350 en BOOTSEL | Pico, Pico 2 | UF2 al disco (hay) y PICOBOOT (lectura) | F6 | F6 | F6 |
| SAMD con bootloader | Arduino Zero, MKR, Adafruit M0/M4 | SAM-BA (BOSSA) o UF2 | F6 | F6 | F6 |

Lo que se graba entra en cualquier formato: `.bin` (con `@0x...` en el nombre
para la dirección), `.hex` (Intel HEX, lo que sale del IDE de Arduino), `.elf`
(los segmentos que se cargan) y `.uf2`. Todo se convierte a una lista de
regiones (dirección, bytes) antes de ir a la familia.

## F1: la base común y el volcado

**Hecha el 2026-10-27** (a9d041a y el arreglo del USB que siguió): leer y
borrar un ESP, en el simulador y en la placa con el reloj AmoledOS por USB
(16 MB leídos en 121 s, grabados de vuelta en 98,5 s). Los formatos están
hechos y probados en la Mac; la lista todavía muestra sólo los `.bin`,
hasta que una familia que los use (SWD, AVR) los pida.

- **Familias en el servicio**: `aos_flasher.c` se queda con el trabajo, el
  hilo, el estado y el registro; cada familia es un archivo con
  `detectar / grabar / leer / borrar / reiniciar` (`aos_flasher_esp.c` el
  primero). El resto de las fases agrega archivos, no toca el núcleo.
- **Formatos**: `aos_flasher_img.c` lee `.bin`, `.hex`, `.elf` y `.uf2` a
  regiones; el simulador lo prueba contra archivos armados en la Mac.
- **Leer (volcado)**: un trabajo nuevo. Elige el rango (toda la flash, o desde
  y hasta), lo guarda en `<sd>/firmware/volcados/<chip>-<fecha>.bin` con un
  `.json` al lado (chip, revisión, MAC o ID, rango, SHA-256, de qué puerto) y
  lo lee dos veces si se pide verificar. El volcado aparece después como un
  firmware más para volver a grabarlo (en la misma dirección).
- **ESP**: leer la flash (`esp_loader_flash_read`, del stub) y borrarla entera.
- **App y portal**: "Leer" junto a "Grabar"; la lista de volcados; el portal
  los baja.

## F2: SWD

**Grabar, verificar y la consola RTT, hechos en el simulador el
2026-10-29; grabar y verificar, probados en la placa con la Pico del
usuario el mismo día** (falta la consola RTT en la placa y la Ekobadge). Lo que quedó,
distinto de lo planeado abajo:

- **Sin `swd.a` en `modules.txt`:** SWD usa los pines de un puerto UART
  del header (TX = SWCLK, RX = SWDIO, su EN = NRST). El mismo cable sirve
  para las dos cosas y no hizo falta tocar el parser.
- **El motor** es `aos_io_swd_*` (`aos_io_pulse.c` y su backend
  `aos_io_pulse_p4.c`): bit-bang por `gpio_ll`, cada paquete en una
  sección crítica, el reloj por un lazo de espera (~1 MHz pedido, **sin
  medir**). En el simulador, `sim/swd_sim.c` emula el objetivo a nivel de
  paquetes (`P4_SIM_SWD=nrf52840|stm32f103|rp2040`, `:protected`).
- **Sin algoritmos de flash cargados en el objetivo** (ni el pack de
  probe-rs): cada familia se maneja desde acá por el bus. nRF52 por su
  NVMC, STM32F0/F1 por su controlador (media palabra por vez, lento pero
  simple), RP2040 por las funciones de su ROM llamadas con el trampolín
  de depuración. Los STM32F4/G0/L4 se reconocen y dicen que todavía no.
  El pack de probe-rs queda para cuando haga falta velocidad o más chips.
- **Multidrop** (RP2040): si nadie contesta tras JTAG-a-SWD, se manda
  SWD-a-dormido, el despertar, un reset de línea y TARGETSEL del núcleo 0.
- **Protección:** un nRF52 con APPROTECT y un STM32 con RDP no se graban;
  el nRF52 se puede borrar entero por su CTRL-AP (se va el firmware con la
  protección). El RDP del STM32 (option bytes) todavía no.
- **Consola RTT** (no estaba en el plan): busca el bloque "SEGGER RTT" en
  la RAM con el objetivo corriendo (sin detenerlo), lee su buffer de
  subida cada 30 ms al registro del Programador y escribe en el de bajada
  lo que llega por `POST /api/flash/console/send`. En la app, "Consola RTT"
  en lugar de "Leer la flash", y mientras está abierta la barra de abajo
  es una línea para escribirle (con el teclado de la pantalla; acostada,
  el teclado ocupa sólo la columna izquierda y el registro sigue a la
  vista).
- Leer la flash entera por SWD sigue afuera (ver "Alcance de SWD").

Lo planeado era:

- **Puerto `swd.a`** en `modules.txt`: `port swd.a swclk=3 swdio=4 nrst=5`. Por
  defecto, los mismos pines de `uart.b` (pines 12, 14 y 11 del header): el
  mismo cable de 4-6 hilos sirve para las dos cosas. GND siempre; 3V3 sólo si
  el objetivo no tiene alimentación propia.
- **El motor** (`aos_swd`, en aos_io): SWD por software con acceso directo a
  los registros del GPIO (la meta: 2-4 MHz, sin interrupciones durante cada
  transferencia); en el simulador, un objetivo emulado (abajo).
- **ADIv5**: reset de línea, JTAG→SWD, despertar del modo dormido y
  **TARGETSEL multidrop** (DPv2, lo necesita la RP2040: núcleo 0, núcleo 1 y
  el DP de rescate), DPIDR, MEM-AP con autoincremento, detener el núcleo
  (DHCSR), reset por AIRCR o por NRST, CPUID y tabla ROM.
- **Identificar el chip** por sus registros: DBGMCU_IDCODE (STM32), FICR
  (nRF52), SYSINFO (RP2040), DSU (SAMD). Se muestra como hoy el ESP: chip,
  revisión, flash, ID único.
- **Verificar**: lo recién grabado se lee por MEM-AP y se compara. Si el
  chip tiene la protección de lectura puesta, se dice y no se sigue (ver
  "Alcance de SWD").
- **Grabar**: con algoritmos de flash que corren en la RAM del objetivo (el
  formato de los `.FLM` de CMSIS: Init, EraseSector, ProgramPage, UnInit).
  Vienen de los objetivos de **probe-rs** (YAML, Apache-2.0/MIT, traen las
  instrucciones del algoritmo y el mapa de la flash): `tools/swd_targets.py`
  los convierte a un pack (`<sd>/swd/targets.pak`) que arma el CI como asset
  del release, no va a git. Primero RP2040, nRF52832/52840, STM32F0/F1/F4/G0/L4
  y SAMD21; con el pack entero, cualquier chip que probe-rs conozca.
- **Simulador**: un objetivo SWD emulado (DP, MEM-AP, memoria, un controlador
  de flash) que reconoce las llamadas al algoritmo por la dirección del PC y
  hace la operación él mismo: prueba todo el camino de la placa sin placa.
- **En la placa**: con la RP2040 del usuario (multidrop, el caso difícil) y
  después la Ekobadge 2026 (SWD directo).

## F3: Arduino por su bootloader

**Hecha en el simulador el 2026-10-29** (falta la placa: un Uno o Nano
por el header y por USB, un Leonardo por USB). `aos_flasher_avr.c`, el
tipo "Arduino" en la app y en el portal (`"family":"avr"`), y
`tools/fake_avr_boot.py` con los cuatro bootloaders (optiboot,
ATmegaBOOT, el del Mega y Caterina), que sólo contestan a su velocidad (la
leen del termios del pty). Probado con los cuatro: detectar, grabar un
`.hex` con verificación, negarse al `with_bootloader`, leer, borrar y
grabar de vuelta un volcado, byte por byte. Decisiones:

- El protocolo se busca probando (v1 a 115200, v1 a 57600, v2, AVR109 a
  57600), con un reset antes de cada intento; el VID:PID de un Mega pone
  v2 primero, el de un Leonardo/Micro/Pro Micro va directo al toque.
- El bootloader no dice cuánto ocupa: se reserva lo de cada tipo (512 B
  optiboot en 32 KB, 1 KB en chips más grandes, 2 KB ATmegaBOOT, 4 KB
  Caterina, 8 KB el Mega). Un `.hex` que llega ahí se rechaza; un volcado
  se graba sin esa parte.
- Borrar: Caterina tiene su borrado; en los STK500 se escribe 0xFF sobre
  el programa (el borrado del bootloader del Mega no anda en todas sus
  versiones). En un Mega son ~25 s a 115200.
- Verificar: cada página leída y comparada (no hay suma en el protocolo);
  un volcado lee cada página dos veces.
- Un Arduino de 5 V por el header necesita un divisor en su TX: lo dice
  la tarjeta de conexión.
- Sin probar: el toque a 1200 baudios en el host USB de la placa (si el
  driver CDC-ACM manda SET_LINE_CODING y DTR como Caterina los espera, y
  el índice en que vuelve el bootloader).

Lo planeado era:

- **STK500 v1** (optiboot y el viejo de los Uno/Nano/Pro Mini): reset por DTR
  (USB) o por la línea EN del header, firma del chip, grabar, verificar y leer
  la flash por páginas.
- **STK500 v2** (Mega 2560).
- **AVR109 / Caterina** (Leonardo, Micro, Pro Micro): abrir y cerrar a 1200
  baudios lo manda al bootloader, que aparece como otro dispositivo USB por
  ocho segundos.
- La placa se reconoce por el VID:PID del USB cuando lo hay (Arduino, CH340,
  16U2) y por la firma del chip.
- Falsos en `tools/`: un optiboot y un Caterina emulados para probar en el
  simulador.

## F4: STM32 por serie y el puente para la Mac

- **Bootloader de ST (AN3155)**: BOOT0 en la línea BOOT y NRST en EN; 8E1;
  detectar (Get, Get ID), leer, borrar (normal y extendido), grabar,
  quitar la protección de lectura avisando que borra todo.
- **Puente**: un trabajo que tiene el puerto y lo pasa a TCP. RFC2217 para
  esptool y pyserial (`esptool --port rfc2217://p4os.local:4000`), y TCP
  crudo para avrdude (`-P net:p4os.local:4001`) y lo que hable por socket.

## F5: la placa como sonda CMSIS-DAP

- **free-dap** (BSD-3) sobre el mismo motor SWD, por el USB OTG como
  dispositivo CMSIS-DAP v2 (bulk): OpenOCD, pyOCD y probe-rs en la Mac sin
  drivers, para depurar con breakpoints lo que el Programador sólo graba.
- Un modo más del USB (como Teclado o Disco), con los pines tomados a nombre
  del Programador.

## F6: el resto de los caminos

- **ISP** para AVR sueltos y para grabar el bootloader de un Arduino: por
  `spi.a` con RESET en un GPIO; fusibles y lock bits, leídos y escritos con
  confirmación.
- **UPDI** (ATtiny 0/1/2, AVR Dx, Nano Every): por un UART en half duplex con
  una resistencia de 470 Ω a 1 kΩ.
- **RP2040/RP2350 en BOOTSEL**: el UF2 desde el Programador, y PICOBOOT (la
  interfaz de vendedor de la ROM) para leer la flash sin SWD.
- **SAMD**: SAM-BA (BOSSA) por el CDC tras el toque a 1200 baudios.

## Hardware para probar

| Qué | Para |
|---|---|
| La RP2040 del usuario | F2 (SWD multidrop) y F6 (BOOTSEL) |
| Ekobadge 2026 (desde el sábado 2026-10-31) | F2 (SWD directo; ver qué chip trae) |
| Una placa ESP32 cualquiera | F1 (volcado) |
| Un Arduino Uno / Nano y un Leonardo, si hay | F3 |
| Una Blue Pill, si hay | F4 y F2 |

## Alcance de SWD (decidido el 2026-10-27)

Pedido del usuario: SWD para **grabar y verificar placas propias** (la
RP2040, la Ekobadge 2026), como hace cualquier sonda (OpenOCD, probe-rs).

Lo que no se hace, y por qué:

- **Nada que saltee una protección de lectura** (RDP de STM32, APPROTECT de
  nRF52, el bit de seguridad de SAMD, cosas así): ni glitching, ni trucos de
  depuración para sacar el firmware de un chip que se protegió a propósito.
  La protección la puso quien hizo el firmware, y saltearla es extraer algo
  que no es nuestro, aunque el aparato sí lo sea. Si un chip está protegido,
  el Programador lo dice y ofrece sólo el borrado total del propio chip
  (que se lleva el firmware protegido junto con la protección).
- **Volcar la flash por SWD queda fuera por ahora.** Sí se lee lo justo
  para verificar lo que se acaba de grabar. El volcado de ESP por serie de la
  F1 sigue como está.
- Al hacer este trabajo, dos respuestas de Claude se cortaron por un filtro
  automático de seguridad, que no dice qué lo disparó. No hay un motivo más
  preciso que dar que el de arriba; el foco se achicó a grabar y verificar
  para seguir sin cortes.

## Reglas

- Las apps son para cualquiera: nada ajustado a las placas del usuario;
  protocolo estándar y el control manual a mano (rango, velocidad, reset).
- PSRAM primero: los buffers de bloque y los algoritmos van a PSRAM; el
  motor SWD no reserva RAM interna salvo su pila.
- Todo borrado o desbloqueo que pierde datos pide confirmación, en la placa y
  en el portal.
