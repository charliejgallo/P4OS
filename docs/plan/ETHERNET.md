# P4OS — Ethernet por cable

La placa no trae Ethernet. Se le suma con un módulo W5500 por SPI en el
header de 40 pines: hoy con cables, después en la placa auxiliar
([PCB.md](PCB.md)). Como el módulo puede no estar, **Ethernet viene apagado
y se prende desde Ajustes**; nada del sistema lo da por supuesto.

**Estado (2026-11-11): fases 0, 1 y 2 cumplidas en la placa.** El módulo,
cableado al header como en el dibujo, contesta por SPI, y con el firmware
de la rama `ethernet` la placa negocia el enlace, toma dirección por DHCP y
sirve el portal por el cable (abajo, "Lo medido"). La convivencia (fase
2) también: el cable cuenta como red, la placa arranca y sigue sin Wi-Fi,
una app que abre el SPI lo pausa y vuelve solo, y la red por cable se
confía con un interruptor. De la interfaz (fase 3) está escrito y visto
en el simulador: Ajustes, **Redes** (por dónde sale la placa y cada
conexión) y **Ethernet** (el interruptor, el estado, el reloj del SPI, la
confianza), el ícono del cable en la barra, y el aviso en Bus antes de
pausarlo. En la placa: instalado, Ethernet arriba y el ícono del cable en
la barra (visto en una captura de su pantalla); el usuario recorrió Redes,
Ethernet y el aviso de Bus en la placa y se ven bien. **El portal** tiene
a Ethernet en su página "Wi-Fi y Ethernet" (estado, prender y apagar, el
reloj; la confianza no, que se cambia sólo en la placa) y **la app EEPROM**
avisa, donde se elige el puerto, que usarlo pausa a Ethernet: los dos
vistos en el simulador e instalados en la placa (la página nueva es la
que sirve; falta que el usuario la recorra).

**Corrida larga, empezada el 2026-11-11 a las 20:07** con este firmware:
Ethernet a 20 MHz, un enlace desde el arranque, 197 915 bytes de RAM
interna libres a los 42 s.

**Leída el 2026-11-12 a las 09:10, 13 h 3 min después: pasó.** Encendida
46 971 s sin reinicios; el enlace arriba 46 968 s, o sea desde el
arranque, y **levantado una sola vez** (`links` 1): no se cayó en toda la
noche. 100 Mbit/s dúplex completo, sin error y sin pausa. En el registro,
de Ethernet sólo están las líneas del arranque; ningún error de red, del
SPI ni del vigilante. La placa siguió con lo suyo mientras tanto (avisos
de las impresoras, Bluetooth). RAM interna libre: 193 311 bytes, 4 604
menos que a los 42 s, y lo mismo en dos lecturas separadas por 47 s; con
una sola lectura de base tan temprana no se sabe si es algo que se pidió
una vez (el audio de los avisos se abrió después) o una pérdida lenta: a
ese ritmo serían unos 350 bytes por hora. Otra lectura más tarde lo dice.
**Lo dijo: a las 23 h 40 min encendida seguían libres los mismos 193 311
bytes.** No es una pérdida: fue algo que se pidió una vez.

Visto de paso, y no es de Ethernet: el registro tiene 772 veces "1-wire:
reset bus failed: no devices found", una por minuto, de Sensores leyendo
un bus 1-Wire sin nada conectado. Tapa el resto del registro: conviene
avisarlo una vez.

**En `main` desde el 2026-11-12** (0.26.0): la rama `ethernet` se unió
después de esa corrida. Lo que sigue (fase 4 en adelante) va en `main`
como el resto del firmware.

**Fase 4 empezada y primera herramienta de la fase 5, 2026-11-12, en la
rama `ethernet` (sin instalar en la placa):** la pestaña **Cable** de Red,
el probador de boca. Visto en el simulador con un switch de ejemplo; en la
placa falta todo, y en particular lo que sólo ella puede decir: si el
W5500 con el filtro de direcciones apagado entrega las tramas de LLDP y
CDP, y lo que dice un switch de verdad. Abajo, "El probador de boca".

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
| **2. Convivencia** ✅ 2026-11-11, salvo lo anotado | Ruta por defecto, el vigilante de red, mDNS y SNTP por el cable, sacar y poner el cable, arrancar sin Wi-Fi, arrancar sin módulo | El portal, la OTA y SSH andan por cable con el Wi-Fi apagado; sacar el cable no reinicia ni cuelga; 12 h prendida sin perder el enlace |
| **3. Interfaz** ✅ 2026-11-11 | Ajustes → Redes; ícono en la barra; `GET/POST /api/eth` y la página Red del portal; IP fija optativa; el módulo en Módulos | Se prende, se apaga y se ve el estado sin tocar un archivo; en el simulador, un Ethernet falso (`P4_SIM_ETH=1`, o `nolink` para el módulo sin cable) |
| **4. Para las apps**, empezada 2026-11-12 | Las apps ya usan sockets y no cambian. Lo nuevo: lista de interfaces con su estado para las herramientas de red (`aos_hal_netif_*`), elegir por cuál sale un socket, modo promiscuo y tramas crudas para las de diagnóstico | Red (la app) muestra las dos interfaces; Modbus TCP, Art-Net y SSH probados por cable. **Hecho:** la lista de interfaces (`aos_hal_netif_list`), los datos del DHCP (`aos_eth_dhcp`) y la escucha del anuncio del switch (`aos_eth_listen`, `aos_eth_neighbor`), exportados a las apps de la tarjeta. **Falta:** elegir por cuál interfaz sale un socket, y las tramas crudas para una app (hoy el firmware sólo guarda el anuncio) |
| **5. Apps de taller** | Las de abajo, de a una. La 1, el probador de boca, está en el simulador (2026-11-12) | Cada una con su prueba en el banco |
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

**Fase 2, 2026-11-11**, con el firmware de la rama y el mismo banco:

- **El cable es la red.** Con el Wi-Fi conectado a la vez, la dirección que
  informa el sistema (`aos_hal_net_ip`, `/api/info`) es la del cable, que
  llegó a los 6 s del arranque; el Wi-Fi, a los 10 s.
- **Wi-Fi apagado en la placa:** sigue contestando por el cable, con su
  hora; `p4os.local` se resuelve a la dirección del cable; la del Wi-Fi deja
  de contestar. Ping de 9 a 26 ms, sin pérdidas.
- **Arranque sin Wi-Fi** (RESET con el Wi-Fi apagado): vuelve por el cable
  con la misma dirección, contesta el ping y el nombre; 80 pings en 80 s sin
  perder ninguno, o sea que no reinició pasado el primer minuto.
- **El portal, en ese arranque, manda a HTTPS y pide la contraseña** (302 y
  después 401). No es una falla: las redes de confianza se reconocen por el
  nombre del Wi-Fi (`aos_access_is_trusted(aos_hal_net_ssid())`), y una red
  por cable no tiene nombre, así que la placa se cree fuera de casa. Es lo
  seguro por defecto. **Falta decidir cómo se le da confianza a una red por
  cable** (abajo, en las preguntas abiertas); se cambia sólo en la placa.
- **Sacar y poner el cable de red** (18 s afuera): el enlace cae, la red
  pasa a ser la del Wi-Fi en el mismo instante, y al enchufarlo negocia otra
  vez a 100 Mbit/s, toma la misma dirección 1 s después y vuelve a anunciar
  el nombre por el cable. Sin reinicio.
- **No probado:** el caso que el cambio protege en el vigilante de red, que
  es el Wi-Fi habilitado sin poder conectar con el cable puesto (habría que
  apagar el router); y 12 h seguidas.

**Lo que apareció de paso, y no es de Ethernet:** la preferencia del reloj
no se guardaba porque **el almacén de preferencias de la placa (NVS, 24 KB)
está lleno**: 630 entradas usadas de 756 y ninguna disponible; 422 claves
en `p4os`, la mayoría de las apps (84 de Monster Hop, 30 de Turbo, 26 y 19
de Modbus, 24 de Mila...). Con el almacén así no se guarda ningún cambio de
ninguna preferencia, tampoco las del sistema (`boot_bad` falla en cada
arranque). **Arreglado ese mismo día, en `main`:** las preferencias pasaron
a un almacén propio de 256 KB (`docs/BUILDING.md`, "The preferences are in
`prefs`"); en la placa se movieron las 422 claves y la del reloj quedó
guardada tras un reinicio.

## La interfaz trabada, 2026-11-12

La misma corrida, leída a las 19:47 con 23 h 40 min encendida: **el cable
tenía enlace y no pasaba nada por él.** Lo que dice el registro:

- A las 14 h de encendida (hacia las 10:04 y las 10:26, con el usuario
  trabajando en el banco) el enlace cayó dos veces y volvió a los 2 s; la
  segunda vez tomó dirección por DHCP sin problema.
- Nueve horas justas después de esa última dirección (32 400 s: lo que dura
  la concesión de esa red), la dirección se perdió con el enlace arriba. O
  sea que **la concesión no se renovó ni una vez**: la renovación de la
  mitad de la concesión, a las 4 h 30, ya no tuvo respuesta. En algún
  momento entre las 10:26 y las 14:56 la interfaz dejó de pasar tramas, sin
  un solo aviso.
- A las 19:34 el enlace cayó y volvió otra vez, y el DHCP tampoco contestó.
- El Wi-Fi de la placa anduvo todo el tiempo, y al reiniciar la placa el
  cable tomó dirección en 1,6 s: el servidor DHCP estaba y contestaba.

**No se sabe por qué.** Lo que encaja: el W5500 guarda su configuración en
registros que se borran si su alimentación tiembla (cables sueltos en un
protoboard, una mano cerca), y ni él ni el driver de la IDF avisan: el
enlace vuelve solo y el driver sigue sondeando un chip sin zócalo abierto.
No se pudo comprobar: el reinicio borró el estado.

**Lo que se hizo: un vigilante en el driver** (`watch_thread` en
`aos_eth_p4.c`). Cada 5 s, con Ethernet andando, lo arranca de nuevo si el
chip ya no tiene la dirección MAC que se le dio (lo reiniciaron), si el
estado de su zócalo no es el que tenía con tráfico, si la dirección se
perdió con el enlace arriba, o si hay enlace y dirección y no entró una
trama en cinco minutos (una vez por silencio: un cable con un solo equipo
callado no es una falla). Arrancar de nuevo cuesta dos o tres segundos de
red. `/api/eth` dice cuántas tramas entraron (`rx`) y cuántas veces lo
arrancó el vigilante (`restarts`), para que la próxima vez haya con qué
mirar.

**Sin probar:** que el vigilante actúe. No hay cómo reiniciar el chip desde
afuera con el cableado de hoy (RST está al aire); en la placa auxiliar RST
sale a una tira y se va a poder.

## El probador de boca (Red, pestaña Cable)

Una pestaña más de Red, no una app de la tarjeta: necesita las tramas que
llegan al chip, y eso es del firmware. Muestra, de la boca a la que está
enchufado el Ethernet:

- **El enlace:** velocidad y dúplex, o por qué no hay (apagado, sin
  módulo, sin cable, en pausa por una app).
- **Lo que dio el DHCP:** dirección, máscara, puerta de enlace, DNS, el
  servidor y la concesión con su edad. El servidor y la concesión salen del
  registro que lwIP guarda del intercambio; los DNS son los de la pila, una
  sola lista para todas las interfaces: con el Wi-Fi arriba a la vez son los
  del último DHCP que contestó.
- **El switch y la boca:** nombre, boca, descripción de la boca, VLAN sin
  etiqueta, si la boca da PoE, modelo y dirección de gestión, leídos del
  anuncio que el switch manda solo (LLDP cada 30 s; CDP de Cisco cada 60).
  Un switch no gestionable no dice nada, y la pantalla lo dice a los 75 s.
- **La salida**, al tocar "Probar la salida": tres pings a la puerta de
  enlace, resolver `example.com`, y tres pings a lo que resolvió (o a
  `1.1.1.1` si no resolvió, para distinguir un DNS caído de una línea
  caída). Sale por la interfaz que la placa esté usando: el cable si está
  arriba.
- **Las interfaces** que tiene la placa, cada una con su dirección, y cuál
  es la salida.

Cómo escucha al switch: los anuncios van a direcciones de grupo que el
W5500 no deja pasar con su filtro prendido (el driver de la IDF dice que
con filtro sólo entran las multidifusiones de IP). Mientras la pestaña está
abierta, el firmware apaga el filtro (`ETH_CMD_S_PROMISCUOUS`): el chip
entrega todo lo que llega a la boca, y una función propia en la entrada
(`eth_input`, antes de lwIP) guarda el anuncio, descarta lo que es para
otra estación y deja pasar el resto. Se apaga sola a los 5 s de que nadie
la pida. La trama se guarda entera (1,5 KB, en PSRAM) y se lee recién
cuando alguien pregunta, fuera de la tarea de recepción, que tiene 4 KB de
pila. El lector (`aos_eth_announce.c`) no le cree nada a la trama: pasó
tres millones de tramas rotas bajo el sanitizador de direcciones sin una
falla.

En el simulador: `P4_SIM_ETH=1` trae un switch de ejemplo que se anuncia
por LLDP a los 3 s (`cdp`: por CDP; `quiet`: uno que no dice nada), con
tramas de verdad armadas ahí y leídas por el mismo código que en la placa.
La prueba de salida del simulador no sale a la red: sus respuestas son de
ejemplo.

**Falta en la placa:** todo. Y saber si conviene dejar la escucha prendida
más tiempo o bajar el sondeo mientras escucha.

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

## La confianza en la red por cable

**Decidido por el usuario (2026-11-11): un interruptor en la placa.** En
Ajustes, Portal web, "La red por cable: de confianza", apagado de fábrica
y visible sólo con Ethernet prendido. Apagado, lo que entra por el cable se
trata como una red ajena (portal cerrado o con contraseña, HTTPS, sin
anunciar el nombre); prendido, como un Wi-Fi de confianza.

Al escribirlo apareció un agujero que había abierto la fase 2: como "hay
red" pasó a incluir al cable, con el Wi-Fi en una red de confianza **todo lo
que entraba por el cable se tomaba también por de confianza**, fuera la red
que fuera (y con el Wi-Fi recién apagado, por el nombre de la última red).
Ahora cada conexión se juzga por la interfaz por la que entró
(`aos_access_ip_trusted`): la del cable por su interruptor, las demás por
el nombre del Wi-Fi. Lo usan el portal, SSH y el puente serie del
Programador. `docs/SECURITY.md` tiene la tabla.

**En la placa (2026-11-11),** con el Wi-Fi en una red de confianza, el
cable en la misma red y el interruptor apagado: el portal contesta por la
dirección del Wi-Fi (200) y por la del cable manda a HTTPS (302), que es la
red ajena; el nombre `p4os.local` se anuncia sólo por el Wi-Fi y se resuelve
a esa dirección. Con el interruptor prendido en la placa, sin reiniciar: el
portal contesta también por la dirección del cable (200) y el nombre pasa
a anunciarse por las dos.

## El puerto SPI, compartido o no

Pedido del usuario (2026-11-11): que quede claro qué pasa con `spi.a`
cuando Ethernet está prendido, y avisar o apagar Ethernet si otra cosa lo
necesita.

**Lo que hace hoy el código:** Ethernet abre `spi.a` con dueño "Ethernet"
y el árbitro de pines le da los tres hilos a él solo. Mientras está
prendido, **nadie más puede abrir `spi.a`**: Bus (pestaña SPI), la app
EEPROM y el servicio de sensores (una termocupla MAX6675 o MAX31855
declarada ahí) reciben "GPIO52 lo tiene Ethernet". Cambiar el reloj en Bus
no puede afectar a Ethernet porque Bus ni llega a abrir el puerto; y aunque
lo abriera, cada dispositivo SPI tiene su propio reloj: el de uno no cambia
el del otro. Lo que sí pasaría al compartir es otra cosa: mientras otro
dispositivo usa los hilos, Ethernet espera, y una transferencia larga o
lenta (leer entera una memoria flash a 1 MHz) lo deja sin atender varios
segundos.

**Decidido por el usuario (2026-11-11): que Ethernet no le saque el SPI al
banco.** Una EEPROM, una radio LoRa, un lector RC522 o Bus tienen que
poder usar el puerto como siempre. Quedó así, y está escrito:

- **Los sensores comparten.** El servicio de sensores abre su chip en el
  mismo puerto con Ethernet andando (otro chip select, su propio reloj, unos
  bytes por ronda). Los hilos son del que llegó primero y pasan al otro
  cuando ese se va.
- **Una app se lleva el puerto entero.** Cuando Bus, EEPROM o cualquier app
  abre el puerto, **Ethernet se pone en pausa** mientras lo tenga, y vuelve
  solo cuando el puerto queda libre. La pantalla lo dice las dos veces
  ("Ethernet en pausa: spi.a lo usa Bus SPI", "Ethernet volvió") y
  `GET /api/eth` lo informa (`paused`, `paused_by`).
- Mientras dura la pausa la salida a internet vuelve al Wi-Fi, si lo hay.

**En la placa (2026-11-11):** con Ethernet arriba, varios envíos seguidos
desde Bus (pestaña SPI, `00 39 00 00`) contestaron `04`; Ethernet se pausó
una vez, con su aviso, y volvió 8 s después del último envío: enlace a los
2 s, dirección 1 s más tarde. La primera versión volvía al instante: Bus
abre el puerto sólo durante cada envío, la pausa duraba 4 ms y la red se
cortaba 3 s por envío, con el aviso de pausa tapado por el de vuelta. Por
eso espera a que nadie pida el puerto durante 8 s (`RESUME_MS`).

Lo que falta: que Bus y EEPROM avisen **antes** de
abrir el puerto, no sólo cuando ya pausaron (fase 3, con la interfaz);
probar los sensores compartiendo el puerto (hace falta una termocupla); y,
para una app que quiera convivir con Ethernet sin pausarlo (una radio que
escucha todo el día), una forma de pedir el puerto compartido.

## Preguntas abiertas

- ¿"Redes" reemplaza a "Wi-Fi" en la lista de Ajustes, o Ethernet es una
  fila aparte? (Acá se propone lo primero.)
- ¿La IP fija entra en la fase 3 o se deja para cuando alguien la pida?
- ¿Qué pasa con la red propia (el punto de acceso) cuando la salida es el
  cable: sigue igual, o pasa a compartir la conexión por sí sola?
