# P4OS — La placa como hub Zigbee / Thread

Plan del pedido G25 (`docs/internal/PEDIDOS-APPS-2026-11.md`): **una de las
dos estrellas de la versión que sigue a la 0.33.0**, junto con los íconos
nuevos (`ICONOS.md`). Empezado el 2026-11-24.

## La idea

El ESP32-C6 de la placa tiene la radio 802.15.4 (la de Zigbee y Thread) y
hasta ahora sólo hacía de Wi-Fi y BLE para el P4 (esp_hosted, `docs/C6.md`).
Con un firmware propio en el C6 que preste también esa radio, la placa sirve
para:

1. **El taller:** escanear los 16 canales (energía y tráfico, el Wi-Fi de
   al lado), un sniffer con decodificación y captura para Wireshark.
2. **Probar equipos:** unir sensores y lámparas como coordinador, ver sus
   mensajes, atributos, batería y señal, mandarles comandos; puente a MQTT
   o Home Assistant.
3. **Emular un equipo** (pedido del usuario): unirse a una red Zigbee
   existente como sensor, botón, enchufe o módulo de relés, para probar el
   lado del coordinador. Lo mismo con Thread.

Un papel a la vez. Sólo en redes del usuario.

## Cómo hablan el P4 y el C6

No hay un UART entre el P4 y el C6 en esta placa (sólo el SDIO, y el UART
del C6 sale afuera por J7). Los ejemplos de Espressif para Zigbee y Thread
con un P4 (`examples/zigbee`, `examples/openthread` de esp_hosted 3.0.9)
usan el C6 como "RCP" y le piden **un UART dedicado** para los datos de la
radio, y además lo dejan sin Wi-Fi: no sirven tal cual.

Lo que sí trae esp_hosted 3.0.9 es **peer_data**: mensajes propios entre el
P4 y el C6 por el mismo SDIO, un número y hasta 8166 bytes. Por ahí va
todo: el P4 pide (`AOS_154_MSG_CMD`) y el C6 contesta y manda lo que oye.
El protocolo está en `components/aos_154/include/aos_154_proto.h`.

## Etapa 1: escáner y sniffer (hecha el 2026-11-24)

- **El C6** (`c6/main/radio154.c`, firmware `3.0.9-154.1`): el driver de la
  radio de la IDF (`esp_ieee802154`) sin pila Zigbee ni Thread. Escaneo:
  por canal, 8 medidas de energía de 1 ms y después escucha el resto del
  tiempo pedido, contando paquetes. Sniffer: modo promiscuo en un canal; la
  interrupción copia cada paquete a un anillo de 8 KB y una tarea lo manda
  al P4 cada 40 ms, con el estado (paquetes, perdidos) cada segundo. Comparte
  la antena con el Wi-Fi y el BLE por la coexistencia de Espressif.
- **La guarda:** la radio arranca apagada siempre y sólo la prende un pedido
  del P4. Si el C6 se cae dos veces seguidas con la radio prendida, en el
  arranque siguiente la deja apagada hasta que el P4 diga `UNGUARD`. Es para
  que esto nunca deje la placa sin Wi-Fi: un C6 que no habla con el P4 sólo
  se recupera con el cable en J7 (`docs/C6.md`).
- **El P4** (`components/aos_154`): guarda los últimos 4096 paquetes en
  PSRAM (~590 KB, cuando se escucha por primera vez) y escribe la captura en
  `<tarjeta>/zigbee/captura-<fecha>.pcap` con una tarea propia. El formato es
  **IEEE 802.15.4 TAP** (linktype 283): cada paquete con su canal, RSSI y
  LQI, y Wireshark lo abre directo.
- **El FCS:** la radio del C6 verifica el FCS y en esos dos bytes deja el
  RSSI y el LQI (lo dice el driver de la IDF). El P4 lo recalcula al
  recibir, o Wireshark marcaría todos los paquetes como dañados.
- **El portal:** `/api/154` (estado y canales), `/api/154/scan`, `/sniff`,
  `/stop`, `/unguard`, `/hello`, `/api/154/frames?from=N`
  (`components/aos_portal/aos_portal_154.c`).
- **El simulador:** un C6 falso (`sim/aos_154_link_sim.c`) con un barrio
  inventado: una red Zigbee en el 15, una Thread en el 25 y un Wi-Fi que
  sube la energía del 16 al 19. `P4_SIM_154=0` hace de C6 viejo, que no
  contesta.

### Medido en la placa (2026-11-24)

- El C6 nuevo se instaló desde el P4 como siempre y la placa volvió con
  Wi-Fi; el P4 nuevo con el C6 viejo también anda (el pedido se rechaza y
  nada más).
- **Escaneo** de los 16 canales: 250 ms por canal en ~5 s; con 2 s por
  canal se encontró la red Zigbee del usuario (17 paquetes en 2 s en un
  canal, nada en los demás). Más energía en los canales 16 a 19: el Wi-Fi.
- **Sniffer:** 348 paquetes en 30 s (datos, acuses, comandos); en otra
  prueba 918 seguidos, **todos con el FCS bien** y ninguno perdido, la
  captura completa en la tarjeta.
- **El Wi-Fi con la radio prendida:** bajando 8 MB del portal mientras se
  escucha, 0,31 a 0,40 MB/s; con la radio apagada 0,31 a 0,44; con el C6
  anterior 0,40 a 0,47. Ese día la red andaba lenta (en octubre eran 0,8 a
  1,3 MB/s) y el ping iba de 5 a 80 ms: la diferencia está dentro del ruido.
  **Falta medirlo mejor**, alternando varias veces.

## Etapa 2: la pantalla (empezada el 2026-11-24)

**Hecho:** la app del sistema **Zigbee** (`aos.zigbee`,
`components/aos_apps/aos_app_zigbee.c`), probada en el simulador; en la
placa instalada pero sin mirar (la pantalla estaba bloqueada). Dos páginas:
Canales (las barras, los Wi-Fi 1, 6 y 11 encima, escaneo rápido o a fondo,
y el canal con tráfico queda elegido para escuchar) y Escuchar (el canal, la
captura para Wireshark, la lista de lo que se oye decodificado: tipo,
direcciones y PAN, y encima Zigbee NWK o 6LoWPAN). Al salir de la app la
radio se apaga. Textos en los cinco idiomas.

**Hecho después (2026-11-24):** la página **Equipos** (la app y el portal):
cada dirección oída con su PAN, canal, paquetes, RSSI y cuándo; la dirección
larga de una corta sale del encabezado de red de Zigbee cuando lo trae
(`aos_154_devices()`, `GET /api/154/devices`). **La página del portal**
`#zigbee`: los canales como barras, el sniffer con la lista decodificada,
los equipos y las capturas de la tarjeta para bajar. En la red del usuario:
16 equipos en 25 s, 9 con su dirección larga, 235 paquetes sin pérdidas.

**Falta:** mirar la app en la placa, filtros por PAN o por equipo, el
fabricante por prefijo (abajo) y el ícono (hoy el glifo de "access point").

Lo pensado:

- **Canales:** las 16 barras de energía y de tráfico, con los canales de
  Wi-Fi 1, 6 y 11 dibujados encima (para elegir el canal de una red nueva).
- **Sniffer:** la lista de lo que se oye, decodificado: el tipo de trama de
  802.15.4 (beacon, datos, acuse, comando), PAN y direcciones; encima, la
  capa de red de Zigbee (NWK: datos o comando, origen y destino, radio,
  seguridad) o 6LoWPAN de Thread. Filtros por PAN y por dirección, y un
  resumen por dispositivo (cuántos paquetes, RSSI).
- **Captura:** empezar y parar, con el archivo en `/zigbee` y su tamaño.
- **La página del portal** con lo mismo, y el `.pcap` para bajar.

## Etapa 3 y siguientes: unirse a redes (a decidir)

Para ser coordinador, router o dispositivo hace falta una pila Zigbee
(`esp-zigbee-lib`) o Thread (OpenThread). Dos caminos:

- **La pila en el C6** ("Zigbee en el SoC", que el C6 soporta), con el P4
  pidiendo por peer_data cosas de alto nivel: abrir la red, la lista de
  equipos, leer y escribir atributos. Lo más simple, pero el C6 tiene
  ~240 KB de RAM libres con el Wi-Fi y el BLE, y la pila ocupa bastante:
  hay que medir; quizá sin BLE mientras se usa.
- **La pila en el P4** con el C6 como RCP, llevando el protocolo spinel por
  peer_data en vez del UART dedicado. La RAM sobra, pero `esp-zigbee-lib` es
  cerrada y espera un UART: puede no dejarse.

El primer paso es probar el primer camino con el ejemplo de un sensor de
Espressif en el C6. La emulación de equipos (un sensor de temperatura, un
módulo de relés de 1 a 4 canales) sale de ahí casi sola.

### Lo medido para la pila en el C6 (2026-11-24)

- **La pila:** `espressif/esp-zigbee-lib` **2.1.0** (Apache-2.0, binaria,
  la API nueva "ezbee"). Guarda la red en una partición NVS con el nombre
  que se le dé (`storage_partition_name`, "nvs" por defecto): **no hace
  falta cambiar la tabla de particiones del C6**, que sólo se cambia por el
  cable en J7. Trae también la versión para el P4 (`lib/esp32p4`), con el
  radio en el SoC o por UART a un RCP.
- **Compila** como coordinador con el radio del C6, junto a esp_hosted, el
  Wi-Fi y el BLE: la imagen pasa de 1,41 a **1,58 MB** (queda el 10 % de la
  ranura de 1,75 MB); la RAM fija casi no cambia (+2 KB): la pila pide la
  suya al arrancar. Armado en una copia aparte, no en `c6/`.
- **La RAM libre del C6 andando** (el C6 `3.0.9-154.2` la informa en el
  estado, protocolo 2, `c6_heap_free`/`c6_heap_min` en `/api/154`): ~78 KB
  libres con el Wi-Fi y el BLE; **el mínimo, 67 KB**, bajando 8 MB del
  portal mientras escuchaba.
- **La prueba en la placa (2026-11-24, tarde):** un C6 que arranca la pila
  sólo cuando el P4 lo pide (`c6/main/zigbee_test.c`, `AOS_154_OP_ZB_START`,
  `POST /api/154/zigbee`), con la guarda. Lo medido:
  - **La ranura del C6 es de 1,5 MB**, no 1,75: el C6 conserva la tabla de
    particiones de fábrica (cambiarla pide el cable en J7). Con la pila y
    `-O2` la imagen es de 1,66 MB y la OTA se cortó en el límite (el C6
    quedó con su firmware, nada roto). **Con `-Os` y sin los nombres de los
    errores entra: 1,56 MB**, 8 KB de margen (`c6/sdkconfig.zigbee`).
  - **La pila pide ~43 KB al arrancar:** el C6 pasa de ~78 a ~35 KB libres
    (mínimo 27-31 KB) con el Wi-Fi y el BLE; con el BLE apagado, de 109 a
    68 KB.
  - **El Wi-Fi aguanta con la pila adentro:** 0,40 MB/s bajando 8 MB (lo
    mismo que sin ella ese día). Pararla devuelve la memoria y el escáner y
    el sniffer vuelven a andar.
  - **La red no se forma:** la pila arranca (señales "Skip Start Up" y "BDB
    Device Start"), el pedido de formar se acepta (0) con el canal 26 solo,
    pero la señal de "formada" no llega en 60 s, ni con el BLE apagado ni
    con `esp_coex_wifi_i154_enable()`. Pedir la formación desde adentro del
    manejador de señales colgaba: va por `esp_zigbee_task_queue_post`.
    **Falta encontrar por qué**: el próximo paso es el registro de depuración
    de la pila (`CONFIG_ZB_DEBUG_MODE`, si entra) y el sniffer de otra placa
    o del mismo P4 mirando el canal 26 para ver si el C6 transmite.
  - **El registro del C6 llega al P4:** `POST /api/154/c6log {"on": true}`
    pone las líneas del C6 en el registro de la placa como "c6: ..." (su
    puerto serie está en J7). Sirve para todo lo que sigue.
- **El firmware normal del C6 no trae la pila** (`CONFIG_ZB_ENABLED=n`,
  1,40 MB con `-O2`): las releases siguen igual. La variante de prueba se
  compila con `sdkconfig.zigbee` (el comando está en el archivo).
- **El usuario** (2026-11-24): en otro proyecto suyo, Zigbee y Bluetooth
  andan juntos en un C6, pero no los tres con el Wi-Fi.

### Qué equipo es: la base de Zigbee2MQTT (pedido del usuario, 2026-11-24)

Cuando la placa sea coordinador, al unirse un equipo se le leen del cluster
Basic el `manufacturerName` (0x0004) y el `modelIdentifier` (0x0005). Con eso
la app puede decir **qué es** ("Aqara WSDCGQ11LM, sensor de temperatura y
humedad") y **si Zigbee2MQTT lo soporta** y qué expone.

- **La base:** `zigbee-herdsman-converters` (Koenkk, en GitHub y en npm), la
  lista de equipos de Zigbee2MQTT, licencia MIT (confirmarla y el tamaño al
  armarlo). Cada equipo: su huella (`zigbeeModel`, el fabricante), marca,
  modelo, descripción y lo que expone. Son ~4000-5000. Otras públicas para
  lo mismo: los quirks de ZHA (`zha-device-handlers`, Apache-2.0) y la lista
  de compatibilidad de Blakadder.
- **Cómo llega a la placa:** un script en la Mac (como `gen_emoji.py`) la
  baja y arma un índice compacto, huella → marca, modelo, descripción,
  soportado y lo que expone, que va como **parte del sistema en la Tienda**
  (`zigbee/devices.pak` o parecido; unos cientos de KB, a medir) y se
  regenera en cada release. **Sin las fotos de los equipos**: son de los
  fabricantes.
- **Antes, con el sniffer:** la dirección larga de un equipo empieza con el
  prefijo de su fabricante (OUI, la tabla pública del IEEE): "chip Telink",
  "Silicon Labs", "Espressif" sin unirse a nada. Es menos, pero sale ya
  (etapa 2). **La tabla:** el sitio del IEEE (`standards-oui.ieee.org`)
  rechaza las descargas automáticas; hay que bajar `oui.csv` a mano desde el
  navegador y que un script arme el índice (no se esquiva el bloqueo).
  **Preparado (2026-11-24):** `tools/zigbee/gen_oui.py <oui.csv>` arma
  `oui.bin` (registros de 32 bytes ordenados por prefijo, ~1 MB) para
  `<tarjeta>/zigbee/`; el P4 busca ahí el fabricante la primera vez que
  conoce la dirección larga de un equipo (`aos_154_devices()`), y la app y el
  portal lo muestran. **En la placa** (con el `oui.csv` que el usuario bajó
  el 2026-11-24: 40 554 fabricantes, 1,2 MB): de 13 equipos con dirección
  larga, 7 Silicon Labs, 4 Telink, 1 Espressif y 1 "IEEE Registration
  Authority" (un bloque repartido en partes: haría falta también la tabla
  MA-M/MA-S). **Como parte del sistema:** `sys-zigbee-oui` en
  `store/system.json`, asset `zigbee_oui.bin` (la Tienda acepta
  `zigbee/*.bin`); se sube a mano antes de la release (`docs/plan/TIENDA.md`,
  "Dónde quedó").
