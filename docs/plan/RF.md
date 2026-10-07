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
| **1. USB crudo** | `aos_hal_usb_raw_*` en el firmware (control, bulk, stream a un anillo en PSRAM), su gemelo en el simulador sobre libusb (`P4_SIM_USB=1`), librtlsdr en la app con su capa libusb (`apps/rf/main/port/`) | hecho el 2026-10-06; probado en el simulador con la RTL-SDR real (2,048 Msps, 4,0 MB/s, nada perdido); **falta la placa** |
| **2. Espectro y cascada** | FFT de 2048 puntos, 25 por segundo; arrastrar para sintonizar, tocar para centrar, escribir la frecuencia, bandas, ganancia, muestreo, paso | hecho el 2026-10-06 (la misma prueba); falta la placa |
| 3. Escuchar | FM comercial (mono, después estéreo y RDS), AM (banda aérea), NFM (radioaficionados, PMR, marina), por el parlante o la placa de audio USB; silenciador | sin hacer |
| 4. 433/868 MHz | demodulador OOK/FSK a pulsos + decodificadores (sensores, controles), publicarlos en Home Assistant por MQTT | sin hacer |
| 5. CC1101 | módulo de `aos_io`: frecuencia, modulación, RSSI, recibir pulsos por GDO0 con el RMT (como Infrarrojo), aprender y reenviar códigos de los controles propios; barrido de RSSI como espectro grueso. Nada de interferir señales | sin hacer; el usuario tiene una CC1101 de 433 MHz con 10 pines |
| 6. Más fuentes | rtl_tcp (sirve para probar sin la placa y para usar una SDR de otra máquina), grabar y reproducir I/Q en la tarjeta | sin hacer |
| 7. ADS-B | aviones en 1090 MHz (2 Msps), dibujados en Mapas | sin hacer; hay que medir si la placa decodifica a esa tasa |
| 8. Página del portal | la cascada y los controles en el navegador (`apps/rf/web/`) | sin hacer |

## Lo medido y lo que hay que medir en la placa

- **La RTL-SDR del usuario:** RTL2832U genérica ("Generic RTL2832U OEM",
  `0bda:2838`, número de serie 00000001) con un **R820T**, 29 pasos de
  ganancia de 0 a 49,6 dB. En la Mac, `rtl_test -s 2400000` no perdió
  muestras.
- **En el simulador** (2026-10-06): 2,048 Msps a 4,0 MB/s sin pérdidas, las
  FM de la banda a la vista.
- **En la placa, por medir:**
  1. que enumere en 25/27 (High Speed, cables de menos de 15 cm) y en 21/23;
  2. MB/s y muestras perdidas a 1,024, 2,048 y 2,4 Msps (el pendrive leyó a
     7,4 MB/s; 2,4 Msps son 4,8 MB/s);
  3. CPU del trabajador (`/api/sysmon`) y fps de la pantalla;
  4. RAM interna con la app abierta (4 × 16 KB de búferes del host mientras
     corre el stream) y que vuelvan al ir al segundo plano;
  5. desenchufarla con la app abierta y volver a enchufarla.
- Consumo: una RTL-SDR toma ~300 mA de los 5 V del pin 1 y se calienta.

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
