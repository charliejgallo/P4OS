# P4OS — Ethernet por cable

La placa no trae Ethernet. Se le suma con un módulo W5500 por SPI en el
header de 40 pines: hoy con cables, después en la placa auxiliar
([PCB.md](PCB.md)). Como el módulo puede no estar, **Ethernet viene apagado
y se prende desde Ajustes**; nada del sistema lo da por supuesto.

**Estado (2026-11-11): fases 0 y 1 cumplidas en la placa.** El módulo,
cableado al header como en el dibujo, contesta por SPI, y con el firmware
de la rama `ethernet` la placa negocia el enlace, toma dirección por DHCP y
sirve el portal por el cable (abajo, "Lo medido"). Se prende sólo por la
API; falta la convivencia (fase 2) y la interfaz (fase 3). El trabajo de firmware va en la rama
`ethernet` (un árbol de trabajo aparte, `.claude/worktrees/ethernet`), no
en `main`, hasta que ande en la placa.

## El módulo

Un "W5500 Lite" (también "Mini W5500", HW-884), visto en las fotos del
usuario:

- 28,5 × 23,5 mm, con el RJ45 (HanRun HR961160C, con transformador)
  sobresaliendo por un extremo.
- Dos filas de seis pines a 2,54 mm, una en cada lado largo. Serigrafía de
  una fila, desde el frente del RJ45 hacia atrás: **G, G, MO, SCK, CS,
  INT**. La otra, según la página del módulo: **GND, V, V, NC, RST, MI**.
- Sólo 3,3 V, sin regulador, unos 200 mA.
- **Las dos filas están a 20,32 mm** (0,8 pulgadas): en el protoboard del
  usuario caen en las columnas C e I, ocho pasos de 2,54. Es lo que ya
  tenían los zócalos de la placa auxiliar (`ETH_ROWS` en
  `hw/aux-board/place.py`).
- **Falta medir:** a cuánto del borde trasero empieza la primera pata, para
  saber cuánto sobresale el módulo de la placa auxiliar; y confirmar en la
  serigrafía el orden de la fila de V y MI antes de darle tensión.

## Cableado de banco

Al puerto `spi.a` del header, con cables cortos (el SPI va a varios MHz).
El dibujo es `docs/img/bench-w5500.svg` (lo arma `tools/bench_diagrams.py`):

![El W5500 Lite en el header](../img/bench-w5500.svg)

| Módulo | Header | GPIO |
|---|---|---|
| V (cualquiera de los dos) | pin 18, 3V3 | — |
| G / GND | pin 33 o 40, GND | — |
| SCK | pin 38 | 52 |
| MO (MOSI) | pin 34 | 50 |
| MI (MISO) | pin 36 | 51 |
| CS | pin 32 | 49 |
| RST, INT, NC | sin conectar | — |

RST queda en alto por el propio módulo: contestó con RST al aire. INT no
se usa: el driver sondea.

<img src="../img/bench-w5500-photo.jpg" width="360" alt="La placa con Bus en la pestaña SPI: TX 00 39 00 00, RX 00 00 00 04, y el módulo W5500 Lite cableado al lado">

## Decisiones de diseño

- **Apagado por defecto.** Se prende en Ajustes; la preferencia sobrevive a
  los reinicios. Con Ethernet prendido y sin módulo, el sistema lo dice
  ("no contesta en spi.a") y sigue: no reintenta para siempre ni traba el
  arranque.
- **Dónde está el chip lo dice `modules.txt`**, como todo en el header:
  `module w5500 spi.a` (y `cs=`, `int=`, `rst=` si no son los del puerto).
  Sin esa línea, Ajustes ofrece agregarla.
- **El puerto SPI es del árbitro de pines.** El driver abre `spi.a` con
  dueño "Ethernet" por el servicio `aos_io_spi_*`: Bus y Módulos lo ven
  tomado, y otra app no lo pisa. El driver de la IDF (`esp_eth`, W5500)
  acepta un SPI ajeno (`custom_spi_driver`), así que no abre el bus por su
  cuenta.
- **Sin pin de interrupción.** El driver sondea (`poll_period_ms`); el pin
  que ahorra vale más que la latencia. Si algún día sobra un pin, `int=`.
- **Cuál es la conexión activa:** la que tiene enlace e IP, por prioridad
  **Ethernet, Wi-Fi, USB**. Es la ruta por defecto de lwIP (`route_prio`:
  el Wi-Fi tiene 100, el USB 5; Ethernet va con 150, no los 50 que trae por
  defecto la IDF). Sacar el cable devuelve la salida al Wi-Fi sin tocar
  nada. Las dos pueden estar arriba a la vez: el portal contesta por
  cualquiera.
- **Ajustes pasa de "Wi-Fi" a "Redes"**: una página con cada interfaz
  (Wi-Fi, Ethernet, el cable USB, la red propia), su estado, su IP y cuál
  es la salida a internet; el Wi-Fi conserva adentro todo lo que tiene hoy.
  Un ícono en la barra de estado cuando la salida es el cable.
- **"Hay red" deja de querer decir "hay Wi-Fi".** Hoy `aos_hal_net_state()`
  es el Wi-Fi, y el vigilante de `main.c` reinicia tras 60 s sin él. Con el
  cable arriba no tiene que reiniciar, y lo que pregunta si hay red (SNTP,
  las apps, el portal) tiene que oír que sí.
- **Sin puente ni NAT entre el cable y el Wi-Fi** salvo que una app lo pida
  (más abajo): por defecto son dos redes que no se ven a través de la
  placa.
- **La MAC** sale de la del chip (eFuse), con otro último byte que el
  Wi-Fi: el W5500 no trae una propia.
- **Seguridad del portal** ([SECURITY.md](../SECURITY.md)): las "redes de
  confianza" se definen hoy pensando en el Wi-Fi. Una red por cable entra
  con la misma regla (su subred, de confianza o no); se revisa en la fase 3
  y se cambia sólo en la placa, como todo ahí.
- **PSRAM primero:** la pila de la tarea del driver y sus buffers van a
  PSRAM; lo que la IDF exija en RAM interna (los buffers DMA del SPI ya son
  del servicio) se anota en `docs/MEMORY.md` con su medida.

## Fases

| Fase | Qué | Cómo se sabe que está |
|---|---|---|
| **0. El chip contesta** ✅ 2026-11-11 | Cablear al header. Sin firmware nuevo: en Bus, SPI, `spi.a`, CS 49, modo 0, 1 MHz, mandar `00 39 00 00` | El cuarto byte de la respuesta es `04` (el registro de versión del W5500): **devolvió `00 00 00 04`** |
| **1. Enlace** ✅ 2026-11-11 | `aos_eth_p4.c` (en `aos_io`): el driver sobre `aos_io_spi`, sondeando, detrás de una preferencia; DHCP; todo al registro. `sdkconfig`: `ETH_USE_SPI_ETHERNET`, `ETH_SPI_ETHERNET_W5500` | La placa toma IP por el cable y contesta ping. Medir: velocidad a 8, 20 y 33 MHz de SPI con los cables del banco, RAM interna que cuesta, CPU |
| **2. Convivencia** | Ruta por defecto, el vigilante de red, mDNS y SNTP por el cable, sacar y poner el cable, arrancar sin Wi-Fi, arrancar sin módulo | El portal, la OTA y SSH andan por cable con el Wi-Fi apagado; sacar el cable no reinicia ni cuelga; 12 h prendida sin perder el enlace |
| **3. Interfaz** | Ajustes → Redes; ícono en la barra; `GET/POST /api/eth` y la página Red del portal; IP fija optativa; el módulo en Módulos | Se prende, se apaga y se ve el estado sin tocar un archivo; en el simulador, un Ethernet falso (`P4_SIM_ETH`) |
| **4. Para las apps** | Las apps ya usan sockets y no cambian. Lo nuevo: lista de interfaces con su estado para las herramientas de red (`aos_hal_netif_*`), elegir por cuál sale un socket, modo promiscuo y tramas crudas para las de diagnóstico | Red (la app) muestra las dos interfaces; Modbus TCP, Art-Net y SSH probados por cable |
| **5. Apps de taller** | Las de abajo, de a una | Cada una con su prueba en el banco |
| **6. Placa auxiliar** | Los zócalos del módulo con sus medidas reales; ver si conviene el chip integrado en la PCBA | El módulo entra en la placa impresa de prueba |

Cada fase termina con lo medido anotado acá y en `PRUEBAS-PLACA.md`.

## Lo medido

**Fase 1, 2026-11-11**, con el módulo en cables Dupont de unos 15 cm y un
cable de red a un switch doméstico. Las descargas se midieron desde una Mac
que está ella misma por Wi-Fi, así que son un piso, no el techo del cable.

- **Enlace:** 100 Mbit/s, dúplex completo, a los 2 s de arrancar el driver;
  dirección por DHCP 1,0 a 1,6 s después. El driver arranca solo al
  reiniciar con la preferencia puesta.
- **El portal por el cable** contesta en la dirección del cable con el
  Wi-Fi también conectado (las dos en la misma subred).
- **Ping** desde la Mac por Wi-Fi: 5 ms de mínimo, 23 de promedio; al Wi-Fi
  de la placa, 7 y 36. El sondeo cada 10 ms entra en esos números.
- **Descarga de un archivo de 3,2 MB de la tarjeta** (`/api/fs/get`), dos
  veces por reloj, todas con el mismo MD5:

  | Reloj del SPI pedido | El que hace el hardware | Descarga |
  |---|---|---|
  | 8 MHz | 8 MHz | 0,49 a 0,52 MB/s |
  | 20 MHz | 20 MHz | 0,72 a 0,75 MB/s |
  | 33 MHz | 34,3 MHz | 0,68 a 0,87 MB/s |
  | 40 MHz | 40 MHz | 0,88 a 0,89 MB/s |

  El mismo archivo por el Wi-Fi de la placa, ese día: 0,24 a 0,25 MB/s.
  Con cables sueltos anduvo sin errores hasta los 40 MHz, el tope del
  servicio SPI. Subir de 8 a 40 MHz no llega a duplicar: el reloj no es lo
  único que frena (el sondeo cada 10 ms, la tarjeta, el servidor HTTP).
  **Queda por medir:** con la Mac por cable, con el sondeo más corto, y la
  subida.
- **RAM:** prenderlo cuesta 12,7 KB de RAM interna (211 503 a 198 779
  bytes libres) y 3,6 KB de PSRAM. Parte es la pila de la tarea de
  recepción (4 KB) y los dos buffers DMA del puerto SPI; falta ver qué del
  resto se puede pasar a PSRAM.

**Lo que apareció de paso, y no es de Ethernet:** la preferencia del reloj
no se guardaba porque **el almacén de preferencias de la placa (NVS, 24 KB)
está lleno**: 630 entradas usadas de 756 y ninguna disponible; 422 claves
en `p4os`, la mayoría de las apps (84 de Monster Hop, 30 de Turbo, 26 y 19
de Modbus, 24 de Mila...). Con el almacén así no se guarda ningún cambio de
ninguna preferencia, tampoco las del sistema (`boot_bad` falla en cada
arranque). El firmware de la rama ahora lo dice en el registro (`pref: ...
not kept`, y una vez por arranque cuánto hay y de quién). Es un arreglo de
sistema aparte, antes de la fase 3.

## Qué gana el taller con un cable de red

Ordenadas por lo que más resuelve en un banco. Varias necesitan la fase 4.

1. **Probador de boca y de cable.** Enchufar y ver: hay enlace o no, a qué
   velocidad y dúplex negoció, qué ofrece el DHCP (servidor, máscara,
   puerta de enlace, DNS, cuánto dura), si se llega a la puerta de enlace y
   a internet, y **a qué switch y a qué boca está enchufado** (LLDP y CDP,
   que los switches administrables anuncian solos). Es lo que hace un
   probador de red de mano.
2. **Encontrar un equipo de IP desconocida.** Una cámara, un PLC o un
   router enchufado directo a la placa: escuchar lo que dice al arrancar
   (ARP, DHCP, mDNS, SSDP, ONVIF) y probar las subredes de fábrica más
   comunes, hasta dar con su IP y su MAC (y el fabricante por la MAC).
3. **Red de banco.** La placa como servidor DHCP de un equipo enchufado
   directo, con **TFTP** para darle un firmware, NTP para darle la hora sin
   internet, y un receptor de syslog para leer lo que dice. Se junta con el
   Programador ([PROGRAMADOR.md](PROGRAMADOR.md), F7, U-Boot por consola):
   la consola por `uart.b` y el firmware por el cable, que es como se
   recupera un router o una cámara.
4. **Compartir la conexión** (NAT de lwIP), en los dos sentidos: dar
   internet por cable a un equipo sin Wi-Fi usando el Wi-Fi de la placa, o
   levantar la red Wi-Fi propia de la placa con salida por el cable (un
   punto de acceso de emergencia en el taller).
5. **Lo que ya existe, por cable.** Modbus TCP y su pasarela a RS485,
   Art-Net y sACN en DMX y Luces, las cámaras, SSH, el puente serie del
   Programador: todo eso anda hoy por Wi-Fi y con el cable deja de depender
   de la señal. Una pasarela Modbus o un nodo Art-Net fijos en un tablero
   quieren cable.
6. **Mirar el tráfico.** Capturar a un `.pcap` en la tarjeta lo que llega a
   la boca (con un puerto espejo en el switch, todo), con contadores:
   broadcast por segundo, dos servidores DHCP en la misma red, dos equipos
   con la misma IP, una tormenta. Un solo puerto no se pone en el medio de
   un cable: ve lo que el switch le manda.
7. **Medir un enlace.** Un servidor y un cliente de iperf, y la comparación
   lado a lado del cable contra el Wi-Fi desde el mismo lugar (latencia,
   pérdida, velocidad): sirve para decidir si un equipo necesita cable.
8. **Wake-on-LAN** y un vigía: lista de equipos del taller con ping o
   puerto, quién está prendido, despertarlos.

Con el Wi-Fi solo, sin cable, ya salen la 7 (parte), la 8 y la mitad de la
2 (lo que se anuncia por mDNS y SSDP): se pueden adelantar.

**Límites que conviene saber:** el W5500 por SPI da del orden de 10 a
20 Mbit/s, no 100; es un solo puerto; no entrega ni toma PoE; y el modo
promiscuo ve sólo lo que llega a esa boca.

## Preguntas abiertas

- ¿"Redes" reemplaza a "Wi-Fi" en la lista de Ajustes, o Ethernet es una
  fila aparte? (Acá se propone lo primero.)
- ¿La IP fija entra en la fase 3 o se deja para cuando alguien la pida?
- ¿Qué pasa con la red propia (el punto de acceso) cuando la salida es el
  cable: sigue igual, o pasa a compartir la conexión por sí sola?
