# P4OS — Placa auxiliar del header

Una placa que se enchufa en el header de 40 pines y lleva cada puerto de
P4OS a un conector con nombre: los dos USB host, los dos I2C (a 3,3 V y a
5 V), RS485, el puerto del Programador con SWD, SPI, Ethernet, CAN,
infrarrojo y un pin de uso general. Hoy eso es un mazo de cables Dupont y
una tapa impresa con dos USB.

**Estado (2026-11-11): borradores, nada fabricado.** Los zócalos del
módulo W5500 están ubicados con la geometría del módulo que anduvo en el
banco, y hay un primer trazado hecho por el script (los cuarenta pines del
header al zócalo de al lado y un plano de masa por cara, DRC de KiCad sin
errores, 111 conexiones por unir): ver `hw/aux-board/README.md`. El mapa de
pines quedó cerrado el 2026-11-10 (`docs/img/pinout.svg`, [EXPANSION.md](EXPANSION.md)) y salió con la
0.25.1. Hay un primer esquemático de la variante casera en
`hw/aux-board/`, y otro de la PCBA sobre las mismas 54 redes (71 y 73
partes, ERC de KiCad sin observaciones), cada uno con una primera ubicación
de las partes en la placa, la serigrafía con el nombre de cada pin y la
máscara negra con que se van a pedir; sin
trazado todavía. **Decidido por el usuario ese día:** se
hacen las dos variantes, la casera (placa de dos capas pedida a JLCPCB,
soldada a mano) y la PCBA con SMD; `i2c.board` sale sólo a 3,3 V; el
adaptador de nivel a 5 V va en `i2c.ext`, que también tiene sus tiras de
3,3 V; **RS485 y CAN van con módulos externos**, no con chips en la placa:
el "RS485 a TTL" automático del banco (3V3, TX, RX, GND) y un SN65HVD230
(3V3, GND, CTX, CRX), cada uno en su tira de pines. Todo lo de abajo que nombra una pieza concreta es una
propuesta a revisar contra su hoja de datos y el stock del fabricante al
dibujar el esquemático: nada de esto se armó ni se midió.

## Reglas

- **Para cualquiera**, como las apps: nada ajustado al banco del usuario.
- **Nada fijo que no se pueda soltar.** Cada bloque soldado a la placa
  (infrarrojo, Ethernet, las resistencias de pull-up) llega a sus GPIO por
  puentes; RS485 y CAN son módulos que se sacan: quitado el
  puente, el pin vuelve a ser un pin libre en una tira de al lado. El mapa
  de pines es un perfil por defecto, no una ley, y la placa no lo tiene que
  convertir en una.
- **Sólo 3,3 V en las señales del P4.** Lo que habla a 5 V pasa por un
  adaptador de nivel o un divisor que está en la placa.
- **El 5 V del header está vivo con la placa apagada**
  ([HARDWARE.md](HARDWARE.md), riesgo 5). El 5 V que sale a los conectores
  pasa por una llave que habilita el riel de 3V3: apagada la P4, se apaga
  lo colgado.
- **GPIO46-48 son señales, no alimentación:** van por el LDO VO4, el de la
  microSD (3,25 V medidos).

## Los bloques

| Bloque | GPIO (pin) | Conector | Notas |
|---|---|---|---|
| Header | todos | tira de pines 2×20 de 2,54 hacia abajo, y un zócalo 2×20 igual al de la placa hacia arriba | El de la placa es un zócalo hembra. Una tira "stacking" (zócalo con pines largos) hace las dos cosas en una sola pieza. Al ir enfrentada, la huella va espejada: se revisa en el trazado |
| 5 V conmutado | pines 1 y 3 | — | Llave habilitada por 3V3; un fusible rearmable por USB |
| USB host "2.0" | USBD_N/P (25, 27) | USB-A hembra | High Speed: par diferencial de 90 Ω, corto y sin vías; protección ESD |
| USB host "1.1" | 24, 25 (21, 23) | USB-A hembra | Full Speed |
| `i2c.ext` 3,3 V | 21, 22 (15, 17) | JST-SH de 4 (Qwiic / STEMMA QT) y tira de 4 | Pull-ups de 4,7 kΩ con puente |
| `i2c.ext` 5 V | los mismos | tira de 4 | Adaptador de nivel de dos canales, pull-ups a 5 V del otro lado; el 5 V es el conmutado |
| `i2c.board` 3,3 V | 7, 8 (4, 6) | tira de 4 | Sólo a 3,3 V, sin adaptador de nivel: es el bus del táctil y del audio, y un sensor que lo cuelga se lleva la pantalla. Ya tiene pull-ups de 2,2 kΩ en la placa |
| `uart.a` / RS485 | 30, 31, 29 (22, 24, 20) | tira de 5: 3V3, TX, RX, GND, DE | El módulo RS485 automático se enchufa en los cuatro primeros; el quinto es DE, para un transceptor que lo pida o para usar el UART pelado |
| `uart.b` + objetivo | 3, 4, 5, 2 (12, 14, 11, 8) | tira de 6: GND, 3V3, TX, RX, EN, BOOT | El cable del Programador y de la Terminal. Divisor con puente en RX para un objetivo de 5 V (el Nano) |
| SWD | 3 = SWCLK, 4 = SWDIO, 5 = NRST | Cortex Debug 2×5 de 1,27 (o tira de 5) | Los mismos pines que `uart.b`: es otro conector, no otro puerto |
| `spi.a` | 52, 50, 51, 49 (38, 34, 36, 32) | tira 2×4: 3V3, GND, SCK, MOSI, MISO, CS, y GPIO46 y 47 | Los dos últimos son los de CAN: libres como auxiliares cuando no hay módulo CAN puesto |
| Ethernet | `spi.a` con su CS, 49 (32), por puente | dos zócalos de 6 para un módulo "W5500 Lite" (HW-884) | El módulo que consiguió el usuario: 28,5 × 23,5 mm, seis pines por lado (G G MO SCK CS INT / GND V V NC RST MI), sólo 3,3 V, unos 200 mA. Con el puente puesto es el chip de `spi.a`. INT y RST salen a una tira de 2: la interrupción no va a un GPIO (el driver puede sondear). **Falta el driver en el firmware** ([ETHERNET.md](ETHERNET.md)), las dos filas del módulo están a 20,32 mm, medido |
| CAN | TX 46, RX 47 (35, 37) | tira de 4: 3V3, GND, CTX, CRX | Para un módulo SN65HVD230, que trae su bornera y su 120 Ω |
| Infrarrojo | receptor 28 (16), LED 32 (31) | en la placa, hacia adelante | Receptor de 38 kHz a 3V3; LED por un NPN desde el 5 V conmutado |
| 1-Wire | 34 (28) | bornera de 3: 3V3, dato, GND | 4,7 kΩ a 3V3 con puente. Es un pin de strapping (la fuente del JTAG), que el chip sólo mira si se quemó el eFuse JTAG_SEL_ENABLE; el pull-up elige lo que ya es el valor de fábrica |
| Tira LED / servo | 48 (39) | bornera de 3: 5 V conmutado, dato, GND | 330 Ω en serie; lugar para un capacitor de 470 µF |

Lo que no entra y por qué: PWM en cantidad va por un PCA9685 en `i2c.ext`
(ya soportado), no por pines propios; el ADC no tiene API todavía
(`aos_io_adc_*`); la consola (37, 38) y BOOT (35) no se tocan.

El reparto de 46, 47 y 48 es el punto apretado: los piden CAN, la tira
LED o el servo, y los auxiliares de un LoRa (DIO1, BUSY, RST). Con los
puentes se elige en la mesa; no se pueden tener los tres a la vez. Los
pines sugeridos de cada cosa son los de
[MODULES.md](../MODULES.md#suggested-pins) y `docs/img/pinout.svg`.

## Dos variantes, un esquemático

El esquemático es uno solo. Cambia con qué se resuelve cada bloque:

| Bloque | PCBA (SMD, armada en fábrica) | Casera (thru-hole y módulos) |
|---|---|---|
| RS485 | Tira de pines para el módulo automático | Lo mismo |
| CAN | Tira de pines para el módulo SN65HVD230 | Lo mismo |
| Ethernet | Zócalos para el módulo W5500 Lite en la primera tanda; integrado (W5500, cristal de 25 MHz, RJ45 con transformador) más adelante, si hace falta | Zócalos para el módulo W5500 Lite |
| Adaptador de nivel I2C | Dos BSS138 por bus y sus resistencias | Dos 2N7000 por bus, o el módulo de 4 canales en zócalo |
| 5 V conmutado | Llave de carga con límite de corriente por puerto USB | MOSFET P con un NPN, y un fusible rearmable por puerto |
| ESD del USB | Arreglo de diodos en SOT-23-6 | Sin protección, o el mismo arreglo soldado a mano (es grande) |
| Infrarrojo | Receptor thru-hole igual; NPN en SOT-23 | TSOP38238 o VS1838B; 2N2222 o BC337 |
| Puentes | De soldadura, cerrados de fábrica, se cortan | Tiras de 2,54 con jumpers |
| Pasivos | 0603 | Resistencias de 1/4 W, cerámicos de 5 mm |
| Conectores | Los mismos, thru-hole, soldados a mano | Los mismos |

La **casera** es en la práctica una placa portadora: dos capas, pistas
anchas, todo a mano con un soldador común. Su riesgo es bajo y sirve para
probar lo que una PCBA no perdona: el mapa de pines, dónde cae cada
conector respecto de la carcasa, y el reparto por puentes.

La **PCBA** integra los chips y queda más chica y prolija, pero un error
de huella o de pin se paga con el lote. Para una primera vez con el
servicio conviene que lo que se arme en fábrica sea lo de bajo riesgo
(pasivos, transceptores SOIC, MOSFET, diodos) y dejar el Ethernet en zócalo
de módulo también acá: el W5500 con su RJ45 es el bloque con más piezas,
el único con un cristal y pares diferenciales, y el que no tiene driver
todavía.

Se hacen las dos (decidido el 2026-11-10). Orden propuesto: el esquemático
común, la casera primero para validar el mapa y la mecánica, la PCBA
después con lo aprendido.

## Sobre el servicio de PCBA (a confirmar al cotizar)

Lo que sigue es lo habitual en los servicios chinos de prototipos y cambia
seguido: se confirma en la página del fabricante el día del pedido.

- Se suben tres cosas: los Gerber con el taladrado, la lista de materiales
  (BOM) y el archivo de posiciones (CPL). KiCad los exporta; el plugin
  *Fabrication Toolkit* los deja en el formato de JLCPCB.
- Cada pieza lleva su número de catálogo del armador (en JLCPCB, el de
  LCSC) en un campo del símbolo: se elige al dibujar el esquemático, no al
  final, y mirando que haya stock.
- Hay piezas "básicas", ya cargadas en las máquinas, y "extendidas", que
  suman un cargo fijo cada una. Elegir básicas para los pasivos baja el
  costo más que cualquier otra decisión.
- Armar una sola cara es bastante más barato que las dos: todo el SMD va
  del mismo lado.
- Los conectores thru-hole (header, USB-A, borneras, RJ45) se pueden pedir
  armados, pero es lo caro; soldarlos a mano es media hora.
- El pedido mínimo suele ser de 5 placas, con la opción de armar sólo
  algunas.

## Mecánica

Medido el 2026-11-10 sobre el modelo 3D de la placa de Waveshare (una copia
local en `case/`, fuera de git), con el origen en una esquina del circuito
impreso, mirándolo de atrás:

| Qué | Medida |
|---|---|
| Circuito impreso | 64,51 × 118,49 mm, 1,6 mm de espesor (la pantalla sobresale: 70,7 × 126,9 mm) |
| Agujeros de montaje | cuatro, en (3,75; 3,25), (60,75; 3,25), (3,75; 115,25) y (60,75; 115,25): 57,0 × 112,0 mm entre centros |
| Header de 40 pines | paralelo al lado largo, su eje a 4,42 mm de ese borde; las dos filas en x = 3,15 y 5,69; los pines de y = 14,17 a 62,43 (paso 2,54) |
| Alto del header | su cuerpo llega a 5,2 mm sobre la placa en el modelo |
| Otros componentes altos de ese lado | un conector en x 1,0-6,2, y 80,0-87,7 (3,4 mm de alto) y otro en x 57,9-62,8, y 86,4-92,2 (4,3 mm) |
| USB-C | los dos sobre el lado corto de y = 118,49 |

Dicho por el usuario con la placa en la mano (2026-11-10): **el header es
un zócalo hembra de 2×20**, y **el pin 1 está en el extremo opuesto a los
USB-C**, o sea en y = 14,17, y **los impares son la fila más cercana al
borde** (x = 3,15), que es también lo que da el modelo.

Falta saber, y el modelo no lo dice:

- La altura libre entre la placa y la tapa impresa (`case/v1/`), y por
  dónde salen los USB-A.
- Dónde mira el LED infrarrojo con la placa en su soporte.

## Herramientas

- **KiCad 10** (instalado el 2026-11-10). Sus archivos son texto y
  `kicad-cli` corre el ERC, el DRC y las exportaciones sin abrir la
  ventana: el proyecto va a `hw/aux-board/` en este repo, con los Gerber
  fuera de git.
- **Backplane** (github.com/BackplaneWorks/Backplane): un espacio de
  trabajo para que un agente edite proyectos de KiCad y FreeCAD, con visor
  de placa, esquemático y 3D. Usa su propia versión modificada de KiCad, y
  en macOS sus binarios no están firmados. Queda como opción para mirar el
  avance; el proyecto es KiCad común y no depende de él.

## Lo que sigue

1. Lo que falta de la mecánica de arriba (alturas, salida de los USB-A), y
   **una medida del módulo W5500**: del borde del lado del RJ45 a la
   primera pata (`ETH_PIN1` en `place.py`, hoy 2,5 mm sacados de las
   fotos). Probar el calce con la placa impresa (`fit-test-th-print.svg`)
   y con la hoja a escala (`paper-th.svg`).
2. Decidir si el Ethernet de la PCBA va integrado o en zócalo.
3. El esquemático: la casera está empezada (`hw/aux-board/gen.py`); falta
   revisarla pieza por pieza, el zócalo del módulo W5500 cuando se sepa cuál
   es, y la lista de la PCBA con sus números de catálogo.
4. Probar en el banco, antes de fabricar, lo que cambia de pin: CAN en
   46/47 con un bus real, y un módulo W5500 en `spi.a` con su CS (pide
   el driver en el firmware: `esp_eth` con el W5500, una interfaz más de
   `esp_netif`).
5. Trazado, DRC, y el pedido. El trazado a mano empieza con la ubicación
   congelada: `place.py` arma la placa de cero cada vez y pisa lo dibujado
   en KiCad.
