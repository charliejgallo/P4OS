# Programador universal: el asistente de taller

Plan del 2026-10-27. La placa tiene que grabar, leer y borrar los
microcontroladores comunes del mercado **sin la computadora**, y cuando la
computadora está, prestarle sus pines como sonda. Este archivo dice qué se
hace, en qué orden y cómo se prueba; cada fase, al terminar, pasa a
`docs/MODULES.md` (lo que quedó) y a `PRUEBAS-PLACA.md` (lo medido).

## Lo que hay hoy (0.23.1)

Todo publicado (las fases F1 a F5 y F7 salieron en la 0.23.0). Entre
paréntesis, con qué se probó en la placa; los detalles, en
`PRUEBAS-PLACA.md`.

- **ESP32 y ESP8266 por serie** (`components/aos_flasher`, sobre
  esp-serial-flasher): desde `uart.b` del header con EN y BOOT, o por USB
  (CP210x, CH34x, FTDI, CDC-ACM, la USB-Serial-JTAG propia de los C3/C6/S3/H2).
  Graba carpetas de ESP-IDF (`flasher_args.json`) y `.bin` sueltos, verifica
  por MD5, lee la flash entera y la borra. (Un DevKit por USB y por el
  header hasta 921600, un C3, el reloj AmoledOS de 16 MB leído y vuelto a
  grabar.)
- **Arduino AVR por su bootloader** (F3): STK500 v1/v2 y AVR109, desde un
  `.hex`; leer y borrar el programa. (Un Nano por USB y por el header, un
  Mega 2560 y una Pro Micro por USB.)
- **SWD** (F2): nRF52, STM32F0/F1/F2/F4/G0/G4/L4, SAMD21/D51 (y sus
  parientes D1x/D20/L2x/C2x/E5x), SAM3X/A (el Arduino Due), RP2040 y RP2350;
  grabar, verificar, leer, borrar entero y la consola RTT. (Una Pico, la
  Ekobadge y una Blue Pill; el resto, en el simulador.)
- **STM32 por su bootloader serie** (F4): grabar, verificar, leer y borrar,
  con RDP respetado. (La Blue Pill.)
- **El puente para la Mac** (F4): RFC 2217 en el 4000 y bytes crudos en el
  4001. (stm32loader, esptool y avrdude.)
- **La placa como sonda CMSIS-DAP** (F5): OpenOCD, pyOCD y probe-rs depuran
  por el conector OTG lo que está en los pines SWD. (OpenOCD, gdb y probe-rs
  con la Ekobadge.)
- **U-Boot por una consola serie** (F7): sólo lee la flash entera con los
  comandos del propio U-Boot. (La cámara SC10W, dos volcados iguales.)
- **Pico (RP2040) en BOOTSEL**: es un disco USB; copiar un `.uf2` a `/usb` lo
  graba (`docs/USB.md`). Fuera del Programador.
- **Terminal serie**: monitor de los UART del header (probada a 460800) y los
  USB.

## Familias y caminos

| Familia | Ejemplos | Camino | Grabar | Leer | Fase |
|---|---|---|---|---|---|
| ESP32 / ESP8266 | DevKit, NodeMCU, módulos | ROM serie (hay) | hay (placa) | hay (placa) | F1 |
| Cortex-M por SWD | RP2040, nRF52 (la Ekobadge), STM32, SAMD, SAM3X (Due), GD32 | SWD con 2-3 cables | hay: RP2040, nRF52840 y STM32F1 (placa); STM32F0/F2/F4/G0/G4/L4, SAMD21/D51, SAM3X/A, RP2350 (sim) | hay (placa: nRF52840, STM32F1) | F2 |
| AVR con bootloader serie | Uno, Nano, Pro Mini, Mega | STK500 v1 (optiboot) y v2 (Mega) | hay (placa: Nano, Mega 2560) | hay (placa) | F3 |
| AVR con USB propio | Leonardo, Micro, Pro Micro | AVR109 (Caterina), toque a 1200 baudios | hay (placa: Pro Micro) | hay (placa) | F3 |
| STM32 sin sonda | Blue Pill, Black Pill | bootloader serie de ST (AN3155), BOOT0 y NRST con las líneas EN/BOOT | hay (placa: Blue Pill) | hay (placa) | F4 |
| Desde la Mac, serie | esptool, avrdude, stm32flash | puente RFC2217 / TCP crudo al puerto (placa: stm32loader, esptool, avrdude) | (la Mac) | (la Mac) | F4 |
| Desde la Mac, SWD | OpenOCD, pyOCD, probe-rs | la placa como sonda CMSIS-DAP v2 por el USB OTG (hay, placa: con la Ekobadge) | (la Mac) | (la Mac) | F5 |
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
  `aos_io_pulse_p4.c`): bit-bang, cada paquete en una sección crítica.
  Primero por `gpio_ll` con un lazo de espera calibrado (1005 kHz para
  1000 pedidos; techo ~1,14 MHz). **Desde el 2026-10-31, por los GPIO
  dedicados del P4:** SWCLK y SWDIO son dos canales del núcleo 0 (los
  hilos del HAL están fijos ahí), cada flanco una instrucción CSR, el
  medio ciclo contado en ciclos de CPU y calibrado al abrir (lo que cuesta
  cada medio ciclo además de la espera, medido con dos esperas). La
  dirección de SWDIO sigue en GPIO_ENABLE (sólo cambia en los
  turnarounds). Mismos pines; `gpio_ll` queda de respaldo. Medido con la
  Pico y cables de unos 15 cm: 4000 kHz pedidos dan 4060; el techo es
  ~9 MHz; un DPIDR cada 14 µs a 4 MHz y cada 8 µs al máximo (antes 50),
  200 de 200 bien. El Programador arranca a 4 MHz y, si nadie contesta,
  prueba a 1 MHz sin anotar el primer intento como error; la sonda usa el
  reloj que pide el depurador (pyOCD leyó 64 KB iguales a 1, 4, 8, 12 y
  16 MHz pedidos; se estanca en ~55 KB/s por pyOCD mismo: una ronda de
  USB con la sonda tarda 0,16 ms y 100 lecturas del DP, 1,35 ms).
  Grabar 256 KB en la Pico, con la verificación: 16,7 s a 1 MHz, 11,6
  a 4 y 10,8 a 9 (más allá manda la flash QSPI de la RP2040). PARLIO,
  más rápido todavía, queda sin usar: es una sola unidad, y no haría
  falta. En el simulador, `sim/swd_sim.c` emula el objetivo a nivel de
  paquetes (`P4_SIM_SWD=nrf52840|stm32f103|rp2040`, `:protected`).
- **Sin algoritmos de flash cargados en el objetivo** (ni el pack de
  probe-rs): cada familia se maneja desde acá por el bus. nRF52 por su
  NVMC, STM32F0/F1 por su controlador (media palabra por vez, lento pero
  simple), RP2040 por las funciones de su ROM llamadas con el trampolín
  de depuración. El pack de probe-rs queda para cuando haga falta
  velocidad o más chips.
- **STM32F2/F4, G0, G4 y L4** (agregados el 2026-10-30, sólo en el
  simulador: la Black Pill es lo más usado y el usuario va a comprar una).
  Dos controladoras más: la de F2/F4 en 0x40023C00, por sectores de 16,
  64 y 128 KB (SER y SNB; el segundo banco de un F42x de 2 MB desde 1 MB)
  y de a una palabra (PSIZE x32, que pide 2,7-3,6 V); y la que comparten
  G0, G4 y L4 en 0x40022000, por páginas de 2 KB (4 KB en un G47x de un
  banco) numeradas dentro de su banco, BKER para el segundo (L47x/L49x,
  G0B1, G47x con DBANK), y de a doble palabra. Se borra sólo lo que toca
  la imagen, como siempre. El emulador tiene un F411 (512 KB), un G071
  (Cortex-M0+, 128 KB) y un L476 (1 MB en dos bancos), con sus errores
  (PSIZE, la doble palabra, PROGERR sobre bits no borrados); la prueba
  compara la flash entera contra lo esperado. Lo que sigue sin grabarse
  se nombra igual: F3, F7, H7, L0, L1, L4R, L5, U5, WB, WL.
- **Quitar RDP de un STM32 desde "Borrar"** (el mismo día): el borrado
  total propio del chip, como el ERASEALL del nRF52, que se lleva el
  firmware con la protección (ver "Alcance de SWD"). En un F1, borrar los
  option bytes y escribir la llave 0x5AA5 en RDP (carga al reiniciar); en
  un F2/F4, RDP a 0xAA en OPTCR; en G0/G4/L4, RDP a 0xAA en OPTR, OPTSTRT
  y OBL_LAUNCH, que reinicia el chip (el registro pide tocar Detectar).
- **SAMD21 y SAMD51** (2026-10-30, sólo en el simulador): las Arduino Zero,
  MKR y Nano 33 IoT, las Adafruit M0 y M4, las Seeed XIAO SAMD21; y por la
  misma controladora D10/D11/D20, L21/L22, C20/C21 y E51/E53/E54. El DSU
  dice el chip (DID) y si está protegido (STATUSB.PROT), y el debugger lo
  alcanza en su dirección externa (+0x100) aunque el chip esté protegido.
  El NVMCTRL graba por un buffer de página que se llena escribiendo en las
  direcciones de la flash, y borra filas de cuatro páginas (256 bytes en
  una D21) o bloques de 8 KB (D51). **El bootloader se respeta:** lo que
  cubre BOOTPROT (8 KB en una Zero, 16 KB en una M4) no se graba ni se
  borra; una imagen que lo pisa se rechaza diciendo desde dónde va un
  programa para esa placa, y "Borrar" deja el bootloader. Un `.bin` sin
  `@0x` en el nombre va solo después del bootloader, donde lo enlazan
  Arduino y Adafruit. Sólo el borrado
  total de una con el bit de seguridad (el CTRL.CE del DSU) se lo lleva,
  y lo avisa. El emulador tiene una D21J18A y una D51J19A con su DSU, su
  buffer de página, MANW y LOCKE sobre las filas del bootloader.
- **Leer la flash por SWD** (2026-10-31): el volcado a la tarjeta como el
  de ESP y STM32 (`firmware/volcados`, su `.json` con `"family":"swd"`),
  todo o un rango, por el bus mientras el objetivo corre, cada pedazo
  leído dos veces. La flash de una RP2040/RP2350 es otro chip y su tamaño
  no está en ningún registro: la ventana XIP de 16 MB la repite, así que
  es donde sus primeros 256 bytes vuelven a aparecer (una flash vacía no
  se mide: hay que decir cuánto leer). Un volcado de una SAM grabado de
  vuelta en la misma placa lleva su bootloader: si es el que ya tiene, se
  saltea y se graba el resto. En la app, "Leer la flash" vuelve al lado de
  "Borrar" y la consola RTT pasa al lado de la sonda ("Sonda USB").
- **El volcado de una RP, más liviano** (2026-10-31): en vez de leer dos
  veces, cada pedazo se lee una vez y se compara con la suma de 32 bits
  que hace la propia RP con un canal libre de su DMA y el sniffer (lee
  ese pedazo de flash hacia un registro de sólo lectura, sin frenar el
  núcleo ni tocar su RAM; el canal se devuelve como estaba, y el DMA
  vuelve al reset si el firmware no lo usaba). El registro dice el tiempo
  del cable, el de la tarjeta y cuántos WAIT hizo el chip. En la Pico de
  pruebas bajó de 44,8 a 36,7 s los 2 MB: su flash es el límite (el blink
  de prueba la deja en serie lenta con el oscilador interno, ~2 WAIT por
  palabra), y la suma del DMA lee esa misma flash. Con la Pico corriendo
  como un firmware del SDK (`rapido.uf2`: quad a 125 MHz) los 2 MB salen
  en 9,5 s, sin un solo WAIT: el límite es el cable y la suma no se nota.
- **RP2350** (agregada el 2026-10-29, sólo en el simulador: el usuario no
  tiene una): DP ADIv6 (DPIDR versión 3), el MEM-AP del núcleo 0 en la
  dirección 0x2000 con sus registros en 0xD00; la tabla de la ROM con un
  puntero por arquitectura (se toma el de Arm seguro, en la A1 y en la A0);
  sin trampolín: un stub nuestro en RAM (MSPLIM/PSPLIM a cero, `blx r7`,
  `bkpt`); antes, ACCESSCTRL a su configuración de fábrica (si el firmware
  no lo bloqueó para el depurador, y entonces no se toca), CP7 habilitado y
  `bootrom_state_reset`; al final FC y RA. Todo como OpenOCD (`rp2xxx.c`,
  `rp2350.cfg`). Un `.uf2` de RP2350 tiene que traer su bloque IMAGE_DEF
  para arrancar: el Blink de `tools/swd_test` es sólo de RP2040.
- **Multidrop** (RP2040): si nadie contesta tras JTAG-a-SWD, se manda
  SWD-a-dormido, el despertar, un reset de línea y TARGETSEL del núcleo 0.
- **Protección:** un nRF52 con APPROTECT y un STM32 con RDP no se graban;
  el nRF52 se puede borrar entero por su CTRL-AP (se va el firmware con la
  protección). El RDP del STM32 (option bytes) todavía no.
- **SAM3X/A, el Arduino Due** (2026-11-02; sólo en el simulador, hasta
  probar el Due del usuario): CHIPID en 0x400E0940 (CIDR: un Cortex-M3,
  arquitectura SAM3A o SAM3X, el tamaño de la flash), dos bancos desde
  0x80000, cada uno con su EEFC (0x400E0A00 y 0x400E0C00). Se escribe
  como lo hace el driver at91sam3 de OpenOCD: el latch por las
  direcciones de la flash y "borrar y escribir página" (EWP) por cada
  página de 256 B, con FWS en 6 mientras tanto (un error del silicio:
  con menos, una página puede perder bits; el simulador lo imita y la
  verificación lo agarra). Una página que en el archivo es toda 0xFF se
  borra con un EWP del latch vacío. Antes, las regiones bloqueadas (de
  16 KB) se desbloquean. GPNVM1 dice si arranca desde la flash o desde
  el SAM-BA de la ROM: se pone después de grabar, y borrar entero lo
  vuelve a 0, como el botón ERASE de la placa. El bit de seguridad
  (GPNVM0) se respeta: con él puesto nada contesta por el bus, y el
  Programador dice que el botón ERASE lo libera (borra todo). La consola
  RTT busca en 0x20070000 (96 KB: la SRAM0 espejada y la SRAM1, como
  linkea Arduino). En el Due: el conector "JTAG" de 10 pines a 1,27 mm
  junto al SAM3X (SWDIO 2, SWCLK 4, GND 3/5/9, reset 10, 3,3 V de
  referencia en el 1).
- **Los nRF52 nuevos** (2026-10-31, para la Ekobadge; sólo en el
  simulador): desde ~2021 (errata 249 de Nordic: un nRF52840 desde el
  build code F, un 52832 desde el G, un 52833 desde el B...) cierran el
  SWD en cada reinicio salvo que UICR.APPROTECT esté en HwDisabled y su
  firmware escriba SwDisable al arrancar (nrfx, Zephyr y el SDK de Nordic
  copian ahí el valor del UICR). Un borrado entero vacía el UICR, así que
  después le queda lo mismo que deja `nrfjprog --recover`: el UICR en
  HwDisabled y, con la flash vacía, 24 bytes de arranque que abren el SWD
  y duermen; el próximo firmware los reemplaza. Se reconocen por las dos
  palabras del FICR que lee `nrf52_configuration_249()` de nrfx. El
  borrado también se lleva REGOUT0 (la tensión de los pines de un
  nRF52840 que va por VDDH; vacío es 1,8 V, contra los 3,3 V de nuestro
  SWD): a uno abierto se le repone el suyo, y uno protegido, que no se
  pudo leer antes, avisa si va por VDDH. Detectar muestra del UICR la
  dirección del bootloader y la tensión de los pines si va por VDDH. El
  simulador tiene `nrf52840:rev3` y `nrf52840:rev3-protected` (por VDDH,
  y su CTRL-AP RESET los vuelve a cerrar: el peor caso).
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

**Hecha en el simulador el 2026-10-29**; en la placa, un Nano con el
bootloader viejo por el host USB el 2026-11-03 (detectar, leer, grabar,
verificar, borrar). Falta: un Uno o Nano por el header, uno con
optiboot, un Mega y un Leonardo por USB. `aos_flasher_avr.c`, el
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
- El toque a 1200 baudios en el host USB de la placa, probado el
  2026-11-05 con una Pro Micro: el driver CDC-ACM manda SET_LINE_CODING y
  DTR como Caterina los espera, el bootloader vuelve como otro aparato y el
  Programador lo encuentra; leer, grabar y borrar, y el sketch vuelve solo
  (`PRUEBAS-PLACA.md`).

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

**STM32 por su bootloader serie, hecho en el simulador el 2026-10-29**
(el usuario no tiene un STM32 todavía). `aos_flasher_stm32.c`, el tipo
"STM32" en la app y en el portal (`"family":"stm32"`), y
`tools/fake_stm32_boot.py` (un F103, un F411 con sectores y un G071, con
el borrado clásico o el extendido, y con RDP puesto). Probado: detectar,
grabar borrando sólo las páginas o sectores que toca la imagen (un sector
del medio queda como estaba), verificar, leer, borrar todo, y con RDP
negarse a grabar y quitarla borrando todo. Decisiones:

- BOOT0 por la línea BOOT y NRST por EN: el mismo `module target` de un
  ESP32. 8E1, 115200 y después 57600.
- El mapa de borrado por familia: páginas parejas (F0, F1, G0, G4, L0,
  L1, L4) y los sectores de los F2/F4 (16, 16, 16, 16, 64 y 128 KB, el
  segundo banco de 2 MB desde el 12). F7, H7 y G47x, todavía con borrado
  total, avisando.
- Al final, un reset con BOOT0 bajo; sin las líneas, GO a 0x08000000.

**El puente, hecho el mismo día** (`aos_flasher_bridge.c`): un trabajo
más ("Puente para la Mac" en la app y en el portal, `POST
/api/flash/bridge`) que tiene el puerto y lo presta por TCP hasta que se
cierra. 4000 es RFC 2217 (esptool y pyserial: velocidad, paridad y las
líneas; en el header RTS es EN y DTR es BOOT, como el reset de esptool) y
4001 los bytes solos, con un reset de Arduino en cada conexión porque
avrdude no puede resetear por un socket. Un cliente por vez, y sólo desde
donde el portal abre sin contraseña (`docs/SECURITY.md`). Probado en el
simulador con las herramientas de verdad: `esptool chip_id` y `write_flash`
(MD5 bien) por `rfc2217://` contra `fake_esp_rom.py`, y `avrdude -c arduino
-P net:` grabando y verificando contra `fake_avr_boot.py`; desde una red
ajena, la conexión se cierra y el registro lo dice. En la placa el
2026-11-05 con una Blue Pill y stm32loader desde la Mac por `rfc2217://`
(`PRUEBAS-PLACA.md`), y con un ESP32 DevKit en el header y esptool
(`chip_id`, `write_flash` a 460800), y avrdude por 4001 con un Nano en
el header.

- **Bootloader de ST (AN3155)**: BOOT0 en la línea BOOT y NRST en EN; 8E1;
  detectar (Get, Get ID), leer, borrar (normal y extendido), grabar,
  quitar la protección de lectura avisando que borra todo.
- **Puente**: un trabajo que tiene el puerto y lo pasa a TCP. RFC2217 para
  esptool y pyserial (`esptool --port rfc2217://p4os.local:4000`), y TCP
  crudo para avrdude (`-P net:p4os.local:4001`) y lo que hable por socket.

## F5: la placa como sonda CMSIS-DAP

**Hecho en el simulador el 2026-10-30.** "Sonda para la computadora" en la
app (tipo SWD) y en el portal (`POST /api/flash/probe`): un trabajo que
toma los pines SWD del puerto, como el resto de SWD, y pasa el USB a un
modo más, PROBE, hasta que se cierra. La computadora ve una sonda CMSIS-DAP
v2 en el conector OTG y OpenOCD, pyOCD y probe-rs la usan sin drivers:
breakpoints, paso a paso, las variables, y sus propios algoritmos de flash
para los chips que el Programador no conoce.

- **El firmware de la sonda es propio** (`aos_flasher_dap.c`), no
  free-dap: free-dap mueve los pines por su cuenta, y eso dejaba afuera al
  simulador y al reloj calibrado. Son los comandos de CMSIS-DAP 2.1 para
  SWD como los hace la implementación de Arm (las lecturas de AP diferidas,
  el value match, `DAP_TransferBlock`, `DAP_SWD_Sequence` para el
  TARGETSEL del multidrop) sobre `aos_io_swd_*`, con dos agregados al
  cable: leer una secuencia con SWDIO suelto y cambiar el reloj con el
  puerto abierto (se mide otra vez). Sin JTAG, SWO, UART ni
  `DAP_QueueCommands`; `DAP_Info` lo dice.
- **El USB** (`aos_usb_p4.c`): una interfaz de vendedor, "P4OS CMSIS-DAP",
  con dos bulk de 512 bytes en High Speed; la clase vendor de TinyUSB sin
  FIFOs, así cada comando llega entero y se encola (cuatro, en PSRAM).
  BOS con los descriptores de Microsoft OS 2.0 para que Windows le ponga
  WinUSB sola, con el GUID de CMSIS-DAP v2. El modo no se recuerda al
  reiniciar, y al cerrar la sonda el puerto vuelve al modo que tenía; con
  el modo Disco no abre.
- **También desde Ajustes → USB** (pedido del usuario, el mismo día): una
  fila "Sonda" junto a Teclado y Disco, que abre la sonda del Programador
  en su último puerto del header; elegir otro modo la cierra y después pasa
  a ése. Y **el host de pendrives le presta el conector OTG**: si lo usaba
  (25/27 o los dos), mientras dura la sonda queda en 21/23 o apagado, y al
  cerrarla vuelve como estaba, sin tocar su ajuste.
- **Probado en el simulador con pyOCD de verdad.** El simulador pasa los
  paquetes por TCP (`sim/usb_probe_sim.c`, `P4_SIM_PROBE_PORT`) y
  `tools/swd_test/dap_tcp.py` le da a pyOCD una interfaz sobre ese socket:
  contra el nRF52840, el STM32F103 y la RP2040 emulados (la RP2040 con el
  `rp2040` de pyOCD, multidrop por `DAP_SWD_Sequence`) encuentra el
  núcleo, frena, escribe y lee registros, 1 KB de RAM ida y vuelta, la
  flash, un paso, y la suelta. El emulador ganó para esto una tabla ROM de
  CoreSight, el paso a paso y el TARGETSEL hecho de secuencias.
- **En la placa, el mismo día**, con la Pico del usuario y pyOCD en la Mac
  por el cable OTG: multidrop, frenar, registros, un paso, RAM, y la flash
  a 35,7 KB/s (el Programador graba a ~26 KB/s por el mismo cable). El
  préstamo del OTG también anduvo. Falta OpenOCD con gdb y probe-rs
  (`docs/plan/PRUEBAS-PLACA.md`).

## F6: el resto de los caminos

- **ISP** para AVR sueltos y para grabar el bootloader de un Arduino: por
  `spi.a` con RESET en un GPIO; fusibles y lock bits, leídos y escritos con
  confirmación.
- **UPDI** (ATtiny 0/1/2, AVR Dx, Nano Every): por un UART en half duplex con
  una resistencia de 470 Ω a 1 kΩ.
- **RP2040/RP2350 en BOOTSEL**: el UF2 desde el Programador, y PICOBOOT (la
  interfaz de vendedor de la ROM) para leer la flash sin SWD.
- **SAMD**: SAM-BA (BOSSA) por el CDC tras el toque a 1200 baudios.

## F7: U-Boot por consola (cámaras, routers, decos)

**Hecha el 2026-11-04** para la cámara Orvibo SC10W del usuario
(Hi3518EV200, `camara-sc10w/HANDOFF-P4OS.md`): `aos_flasher_uboot.c`, la
familia "U-Boot" (`"family":"uboot"`). **Sólo lee**: ningún comando que
escriba (ni `saveenv`, ni `sf erase/write`, ni `mw` fuera de la RAM de
trabajo); grabar y borrar contestan que no.

- **Detectar:** el puerto a la velocidad de la consola (115200, o
  `console_baud`), se pide prender o reiniciar el aparato (EN en bajo un
  momento si el puerto lo tiene), y van Ctrl+C y Enter cada 25 ms hasta
  un prompt: una línea corta que termina en `#`, `>` o `=>`, confirmada
  cuando un Enter la repite. `version` tiene que decir "U-Boot" (el `# `
  de un Linux no se confunde). Después `printenv` (bootargs, mtdparts,
  loadaddr), `bdinfo` (si existe) y `sf probe 0`; la flash (nombre, ID,
  tamaño) sale de lo que dicen ellos y el arranque, en las formas de
  cada versión de U-Boot, o de la suma de mtdparts. El arranque y las
  respuestas van a un `.txt` en `firmware/volcados`.
- **Leer:** de a 64 KB: `sf read` a la RAM de trabajo (`ram`, o loadaddr,
  o la DRAM + 32 MB, o 0x82000000), `crc32` de eso y `md.l`, que se
  parsea de vuelta a bytes. Si el CRC no es el de U-Boot, el `crc32` de
  cada 4 KB dice cuáles vinieron mal y sólo esos se repiten; un pedazo
  todo 0xFF ni se imprime. Sin `crc32`, cada pedazo dos veces. Antes, la
  consola sube a 921600 (o 460800, 230400) con `setenv baudrate` y el
  "press ENTER" de HiSilicon, sin `saveenv`, y al final vuelve sola. El
  `.json` del volcado lleva la versión de U-Boot, la flash, mtdparts y
  bootargs; el `.txt` al lado. El aparato queda frenado en el prompt.
- **Simulador:** `tools/fake_uboot.py` sobre un pty (`P4_SIM_UART_B`),
  calcado del U-Boot de la SC10W: 2010.06-dirty, se frena sólo con
  Ctrl+C, sin `bdinfo`, su mtdparts. Probado: los 16 MB byte a byte (72 s
  en el pty), sin `crc32`, con ruido en la línea (repite los 4 KB que
  fallan), un U-Boot que no sube de 115200, y uno cerrado (lo dice y no
  intenta nada). En ningún caso salió un comando que escriba.
- **La cámara del usuario** (2026-11-04, a mano por el portal antes de
  la familia): la consola anda a 115200 una vez cambiada la fuente (con
  la otra no arrancaba bien y llegaba basura); U-Boot 2010.06-dirty
  (Feb 27 2016), prompt `hisilicon # `, **se frena sólo con Ctrl+C**
  aunque diga "any key", entorno "bad CRC, using default", sin `bdinfo`,
  con `crc32` y `md`. Flash MX25L128 de 16 MB (ID c2 20 18), mtdparts
  `512K(boot),2560K(kernel),12800K(rootfs),512K(config)`, rootfs jffs2;
  el kernel se carga en 0x82000000. WiFi por USB con el driver `rt2870`.
- **Frenarla, corregido el 2026-11-05:** el U-Boot de la SC10W no se
  frena con Ctrl+C solo, aunque diga "any key". Le llegaron 644 Ctrl+C a
  12 por segundo y siguió de largo, con o sin microSD. Tampoco con la
  mezcla rápida de Ctrl+C y Enter de la familia. Lo frena la mezcla
  `Ctrl+C, Enter, Ctrl+C, espacio, Ctrl+C, q, Ctrl+C, x, Esc` a unas 12
  teclas por segundo: la buena es espacio, `q`, `x` o Esc. Ayer anduvo
  "con Ctrl+C" porque se mandó esa misma mezcla. **La familia ya la manda**
  (una tecla cada 80 ms, Ctrl+C entre cada una); el U-Boot falso tiene
  `--stop-key` y en el simulador se frena con `q`, espacio, Esc o Ctrl+C. La cuenta dura 0,7 s y llega a los ~5 s de
  prender (antes lee la microSD buscando su propia actualización:
  `config`, `u-boot`, `kernel`, `rootfs`, `rootfsbk`).
- **OpenIPC instalado en la cámara desde P4OS** (2026-11-05): por la
  consola, con el canal A de la Terminal y `/api/serial/send` desde la
  Mac. Pasos:
  - frenarlo con la mezcla de teclas;
  - `fatload` de `openipc.bin` desde la microSD de la cámara a 0x82000000,
    con `crc32` igual al de la imagen (05f1243a);
  - `sf erase` (66 s) y `sf write` (21 s);
  - verificado con la RAM en ceros, `sf read` y otra vez `crc32` 05f1243a;
  - `reset`, y en el U-Boot de OpenIPC `run setnor16m`.

  Arrancó OpenIPC (kernel 4.9.37) y detectó solo el OV9732. La familia
  U-Boot no graba: esto se hizo a mano, con el OK del usuario en cada paso
  que escribía. Detalles en `camara-sc10w/HANDOFF-DESDE-P4OS.md`, sección 8.
- **El tutorial para cualquiera** (2026-11-05), público en
  [github.com/charliejgallo/camara-sc10w](https://github.com/charliejgallo/camara-sc10w)
  con las fotos y el firmware de OpenIPC (con sus créditos),
  con un adaptador USB-serie común y scripts de Python (pyserial):
  - `volcar.py`: el mismo método que la familia, sf read, crc32 y md.l,
    repitiendo los 4 KB que fallan;
  - `armar_imagen.py`, que reproduce la imagen instalada (CRC32 05f1243a);
  - `instalar.py`, que graba desde la microSD de la cámara verificando por
    CRC antes y después, sirve para OpenIPC o para volver a fábrica y hace
    el `setnor16m`.

  Probados de punta a punta contra `tools/fake_uboot.py --writable`:
  volcar, instalar OpenIPC hasta el login y volver a fábrica, la flash
  igual byte a byte.
- **Los volcados de la cámara** (2026-11-04, con la familia, desde el
  portal): dos de 16 MB, 102 min cada uno, cada 64 KB contra su `crc32`,
  **iguales byte a byte** (SHA-256 `1f508d27…`). A 115200 todo el tiempo: su U-Boot contestó "not
  supported" a 921600, 460800 y 230400 (la tabla de velocidades de ese
  2010.06 no las tiene), así que `md` va a ~10 mil caracteres por
  segundo, unos 2,6 KB de flash. Ningún pedazo vacío: el jffs2 marca
  cada bloque, aun los libres. Adentro, lo que dijo al arrancar: U-Boot
  en 0, el uImage `Linux-3.4.35` de 2208808 bytes en 0x80000 (carga en
  0x80008000), jffs2 desde 0x300000 y la config (otro jffs2) en
  0xF80000. Para la próxima, más rápido: la pinza SOIC8 con la app
  EEPROM (ya lee las 25Qxx por `spi.a`), o un `mmc write` de la RAM a una
  microSD en la cámara.
- **Un U-Boot sin `sf` ni `crc32`: el Arduino Yún** (2026-11-05). Su
  Atheros AR9331 corre OpenWrt con un U-Boot 1.1.4 de 2013 (prompt
  `ar7240> `) que sólo tiene `md`, `cp`, `mw`, `erase`, `bootm` y poco
  más; su consola está detrás del 32U4, así que se llega con el sketch
  `YunSerialTerminal` (del IDE de Arduino) por `usb0`, a 250000 aunque su
  entorno diga `baudrate=115200`. Lo que cambió en la familia:
  - Sin `sf`, la NOR está mapeada en memoria: la base sale del primer
    número de `bootcmd` en una ventana de flash conocida (0x9f/0xbf de
    Atheros y Qualcomm, 0xbfc de Broadcom, 0xbc de Ralink, 0x1f/0x1c
    físicas) redondeado al tamaño (`bootm 0x9fea0000` → 0x9f000000), y se
    lee directo con `md`. El tamaño, de "Flash: 16 MB" o de mtdparts.
  - MIPS big-endian: `md.b` contra `md.l` dice cómo dar vuelta las
    palabras (también en el camino con `sf`, por si aparece uno).
  - Sin `crc32`, un voto por línea de `md`: dos lecturas y, donde no
    coinciden, sólo esas líneas otra vez hasta que dos coincidan (leer dos
    veces por bloques de 4 KB no aguantaba ruido en el simulador).
  - La palabra para frenar, si el arranque la nombra ("stop with 'lin'"
    en los de Linino; "tpl" en los TP-Link), se escribe.
  - Un prompt que no contesta `version` como U-Boot (el Linux de un
    aparato que ya estaba prendido) se deja de lado y se espera el
    reinicio, en vez de fallar.
  - Por el USB de un Arduino (un puente) no se cambia la velocidad.

  En el Yún: Detectar en 1,9 s (AR7240 por el prompt, 16 MB mapeados en
  0x9F000000, big-endian); los primeros 256 KB leídos en 91 s (2,8 KB/s,
  con 3 a 12 tramos repetidos por cada 64 KB: el puente pierde algún
  byte), y su MD5 igual al que calcula el Linux del Yún para `/dev/mtd0`.
  Después los 16 MB enteros, solos: vio su Linux, esperó el reinicio, lo
  frenó y leyó todo en 94,5 min; el uImage adentro pasa los dos CRC de su
  encabezado.
  Probado antes contra `tools/fake_uboot.py --atheros` (con `--stop-word`,
  `--linux-first` y ruido).

## Más adelante: los WCH (CH32V)

Pedido del usuario (2026-10-30, "me re interesa"): los microcontroladores
RISC-V de WCH, baratísimos y muy usados en la comunidad: CH32V003 (el de
diez centavos), CH32V002/V006, CH32V203/V208, CH32V307, CH32X035,
CH32L103. No son SWD: se depuran por **SDI**, el protocolo de un solo
cable de WCH (SWIO, más GND; el V003 lo tiene en PD1), el mismo que usa el
WCH-LinkE. Para hacerlo:

- **El cable:** un pin bidireccional con tiempos de bit cortos (el host
  manda pulsos bajos de distinto largo; el chip contesta estirándolos). El
  bit-bang de `aos_io` alcanza para empezar; el RMT o PARLIO del P4 si
  hace falta precisión. Otra familia en `aos_flasher` ("WCH"), con su
  emulador en el simulador.
- **Encima:** el Debug Module de RISC-V (DMI): detener el núcleo, leer y
  escribir memoria por abstract commands o program buffer, y la
  controladora de flash de WCH (parecida a la de un STM32F1, con páginas
  de 64 bytes para escritura rápida en el V003).
- **Referencias abiertas:** `minichlink` de ch32v003fun (MIT) y `wlink`,
  que documentan el protocolo y los registros; el fork de OpenOCD de WCH.
- **De paso:** los CH32F103 de WCH son Arm y hablan SWD: probablemente ya
  anden como un STM32F1, a confirmar con uno.
- **Para probar:** un CH32V003 (placa de desarrollo o el chip con su
  programador) y un CH32V203.

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
- **Volcar la flash por SWD** quedó fuera al principio; **se hizo el
  2026-10-31, a pedido del usuario**, con la misma regla de arriba: es lo
  que hacen OpenOCD, pyOCD o probe-rs con la flash de una placa propia, y
  un chip protegido no se lee (ni se intenta nada para leerlo): el
  Programador lo dice y ofrece sólo su borrado total.
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
