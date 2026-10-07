# RF: radios definidas por software y transceptores

Arrancada el 2026-10-06. Una sola app, **RF** (`apps/rf/`, id `aos.rf`), para
todo lo que sea radio: sintonizadoras que entregan muestras crudas (SDR) y
chips que ya demodulan (CC1101 y parecidos). "Radio" ya era la app de radios
por internet, de ahí el nombre.

## La idea: dos capas de hardware abajo, una app arriba

| | Fuentes de muestras (SDR) | Transceptores |
|---|---|---|
| Qué entregan | I/Q crudo; la demodulación la hace la placa | pulsos o paquetes ya demodulados |
| Primero | RTL-SDR (RTL2832U + R820T) por el host USB | CC1101 de 433 MHz (10 pines, SPI + GDO0/GDO2) |
| Después | rtl_tcp por la red, grabaciones en la tarjeta, Airspy/HackRF | SX127x (LoRa), nRF24, RFM69 |
| Cómo entra | un `rf_src_ops_t` más (`apps/rf/main/rf.h`) | un módulo de `aos_io` (`docs/MODULES.md`), sin firmware si alcanza con SPI y GPIO |

Lo que une las dos capas son **los pulsos**: una señal OOK de 433 MHz (control
de portón, estación meteorológica) demodulada por la RTL-SDR es la misma
secuencia de pulsos que entrega la CC1101 por GDO0. Una sola biblioteca de
decodificadores (al estilo rtl_433) sirve para las dos.

Las apps son para cualquiera (`p4os-apps-generic`): bandas comunes y
estándares, nada de una lista de estaciones ni ajustes al equipo del usuario.

## Fases

| Fase | Qué | Estado |
|---|---|---|
| **1. USB crudo** | `aos_hal_usb_raw_*` en el firmware (control, bulk, stream a un anillo en PSRAM), su gemelo en el simulador sobre libusb (`P4_SIM_USB=1`), librtlsdr en la app con su capa libusb (`apps/rf/main/port/`) | **hecho y probado en la placa el 2026-10-06** (abajo) |
| **2. Espectro y cascada** | FFT de 2048 puntos, 25 por segundo; arrastrar para sintonizar, tocar para centrar, escribir la frecuencia, bandas, ganancia, muestreo, paso | **hecho y probado en la placa el 2026-10-06** |
| **3. Escuchar** | FM comercial (mono; estéreo y RDS después), AM (banda aérea), FM angosta (radioaficionados, PMR, marina), por el parlante o la placa de audio USB; silenciador | **hecho el 2026-10-07**: probado en la Mac con señales sintéticas y en la placa con una FM real (el piloto estéreo medido a 36-43 dB) y la CPU medida; FM oída por el usuario el 2026-10-07 ("se escucha bien"); **faltan AM y FM angosta con señales reales** |
| **4. 433/868 MHz** | demodulador OOK a pulsos + decodificadores (sensores, controles), publicarlos por MQTT | **OOK hecho el 2026-10-07**: EV1527, PT2262, Nexus, Prologue y un analizador para lo demás, CSV por día y MQTT; probado con señales sintéticas y corriendo en la placa (16 % del hilo, 29 % del núcleo 0); **falta un control o sensor real**. FSK (muchos sensores de 868) queda para después |
| 5. CC1101 | módulo de `aos_io`: frecuencia, modulación, RSSI, recibir pulsos por GDO0 con el RMT (como Infrarrojo), aprender y reenviar códigos de los controles propios; barrido de RSSI como espectro grueso. Nada de interferir señales | sin hacer; el usuario tiene una CC1101 de 433 MHz con 10 pines |
| 6. Más fuentes | rtl_tcp (sirve para probar sin la placa y para usar una SDR de otra máquina), grabar y reproducir I/Q en la tarjeta | **grabar y reproducir I/Q, audio WAV y capturas hechos el 2026-10-07** (probados en el simulador); falta rtl_tcp |
| 7. ADS-B | aviones en 1090 MHz (2 Msps), dibujados en Mapas | sin hacer; hay que medir si la placa decodifica a esa tasa |
| **8. Página del portal** | la cascada y los controles en el navegador (`apps/rf/web/`) | **hecha el 2026-10-07**, con un canal en vivo nuevo del sistema (`/api/live`, sirve para cualquier app); probada entera contra el simulador y el canal en la placa (~80 ms por espectro) |

## Lo medido y lo que hay que medir en la placa

- **La RTL-SDR del usuario:** RTL2832U genérica ("Generic RTL2832U OEM",
  `0bda:2838`, número de serie 00000001) con un **R820T**, 29 pasos de
  ganancia de 0 a 49,6 dB. En la Mac, `rtl_test -s 2400000` no perdió
  muestras.
- **En el simulador** (2026-10-06): 2,048 Msps a 4,0 MB/s sin pérdidas, las
  FM de la banda a la vista.
- **En la placa** (2026-10-06, pines 25/27, High Speed):

  | | 2,048 Msps | 2,4 Msps |
  |---|---|---|
  | USB | 4,1 MB/s, 0 perdidas | 4,8 MB/s, 0 perdidas |
  | CPU (núcleo 0 / 1) | 35 % / 15 % | 25 % / 8 % |
  | Pantalla | 25 cuadros/s | 25 cuadros/s |
  | Una FFT de 2048 | 1,35 ms | 1,4 ms |

  Desenchufada con la app abierta: el stream se cortó limpio (194 MB, 0
  perdidas, 2 errores en el instante del corte), la app la soltó y la volvió
  a abrir sola en menos de un segundo al enchufarla otra vez; la RAM interna
  libre quedó igual (121 KB con la app abierta).
- **Lo que costó llegar ahí:** la primera versión dibujaba el espectro y la
  cascada con LVGL (620 000 píxeles desde PSRAM por cuadro) y tenía la FFT en
  PSRAM: 93/97 % de CPU, 12 cuadros/s y 4,3 ms por FFT. Ahora el trazo se
  redibuja sólo donde cambió, las dos imágenes van directo al panel
  (`aos_hal_display_blit_scaled`) y la FFT trabaja en 28 KB de RAM interna.
  La pantalla compite con todo lo que lee PSRAM: un lazo caliente en PSRAM
  rinde un tercio.
- Probada en esa sesión una falla del sistema: la app de abajo de la pantalla
  de bloqueo dibujaba encima de ella (Video, Doom y las otras que van directo
  al panel también). `AOS_UI_OVER_LOCK` lo arregla para todas.
- Sin probar: los pines 21/23 (Full Speed, 0,25 Msps) y 1,024/1,8 Msps.
- Consumo: una RTL-SDR toma ~300 mA de los 5 V del pin 1 y se calienta.

## Escuchar: lo medido y lo aprendido (2026-10-07)

- **El punto flotante del P4 es lento para esto:** ~5 ciclos por
  multiplicación-suma, haga lo que haga el código. En flotante la cadena de
  FM a 960 k tomaba el 72 % de un núcleo. Los filtros pasaron a 16 bits sobre
  el SIMD del P4 con esp-dsp, que el firmware ahora presta a las apps
  (`docs/APPS-P4.md`, "DSP": lo medido y las trampas).
- **Muestreo para escuchar:** 240 k por defecto para AM y FM angosta (la
  RTL2832 hace la primera decimación adentro), 960 k para FM comercial (más
  espectro), 1,92 M opcional. 1,44 y 2,4 M quedan para mirar: su primera
  etapa divide por 3 o 5, y esp-dsp con decimación impar no da lo mismo que
  C.
- **AM y FM angosta a 24 kHz de audio:** la voz no necesita más, y el filtro
  de canal a 48 k en C costaba un 25 % de núcleo.
- **El bloqueador de continua promedia 0,1 s:** con el promedio de cada
  bloque (0,5 ms a 2,4 M) seguía a una portadora corrida 1 kHz y la AM salía
  con el segundo armónico a −27 dB.
- **La cola del parlante:** entre 50 y 200 ms; si el motor se atrasa y se
  pone al día, se tira el bloque (una vez se llenó el segundo entero y no
  bajaba).
- **El motor corre a prioridad 1:** a 3 dejó sin CPU al hilo `tick` del
  sistema y el vigilante reinició la placa.
- Primero se rompió el heap por un búfer de audio chico (el colchón de
  100 ms no entraba): desde entonces todo pasa por el simulador con ASan
  (`sim/build-rf-asan`) antes de la placa.

## Decisiones

- **El driver vive en la app, no en el firmware** (2026-10-06): el firmware
  presta acceso USB genérico y la `.so` trae librtlsdr. Así el firmware
  sigue MIT (librtlsdr es GPL, como el motor de Doom) y una sintonizadora
  nueva es una app nueva, no un firmware nuevo.
- **Formato común cu8** (I/Q de 8 bits sin signo, el de la RTL2832): una
  fuente de 16 bits convertirá o sumará un campo de formato cuando llegue.
- **Los búferes de las transferencias quedan en RAM interna**, como todos los
  del host (en PSRAM leían basura, `docs/USB.md`), y sólo mientras corre el
  stream.
