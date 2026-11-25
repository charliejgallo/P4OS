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
   lado del coordinador. Lo mismo con Thread. El diseño (como router,
   nunca a pila: el P4 sólo tiene la librería de coordinador y router) y
   las etapas están en `ZIGBEE-EMULAR.md` (2026-11-25); la etapa 0 (todo
   menos la placa) está hecha: la pestaña Emular.

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
- **Las ranuras del C6 agrandadas (2026-11-24, noche):** a pedido del
  usuario, el C6 de su placa reescribió su tabla de particiones sin cable
  (`c6/main/slots.c`, `POST /api/154/slots/grow`, `docs/C6.md` "The
  slots"): de 1,5 a 1,94 MB por ranura. Con eso entra la pila compilada con
  `-O2` (1,66 MB, probado: se instaló y corrió). **Pero con `-O2` el C6
  arranca con ~53 KB libres** (el código ocupa más de la RAM interna que
  comparte), **y al arrancar la pila se quedó sin memoria y se cayó**: el P4
  reinició la placa y volvió con el Wi-Fi (la radio apagada al arrancar, como
  estaba pensado). Con `-Os`, 78 KB libres, la pila corre pero no forma la
  red. **Conclusión: en el C6 no entra cómoda junto al Wi-Fi y el BLE**; el
  camino es la pila en el P4 con el C6 de radio.
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

### La pila en el P4, con el C6 de radio (empezado el 2026-11-24, noche)

- **El túnel de spinel anda** (`3.0.9-154.5`, protocolo 5): el C6 corre el
  RCP de OpenThread de la IDF (`c6/main/rcp.c`), y su spinel, en vez de un
  UART, va y viene por peer_data: `otPlatUartSend` está envuelto con
  `-Wl,--wrap` y lo que llega del P4 entra por `otPlatUartReceived` con el
  candado de OpenThread. Esp_hosted tiene esto previsto ("spinel over the
  ESP-Hosted transport") pero "not implemented yet". **Probado en la placa:**
  el P4 le pide la versión en spinel (`GET /api/154/rcp/probe`) y contesta
  `openthread-esp32/...; esp32c6`.
- **El C6:** la imagen con el RCP es de 1,49 MB (entra en un C6 de fábrica);
  le quedan 53 KB libres con el RCP enlazado y 37 KB con el RCP andando
  (contra ~35 KB con la pila Zigbee entera adentro, que además no cabía
  cómoda). El RCP arranca sólo a pedido (`POST /api/154/rcp`) y queda hasta
  que el C6 se reinicia.
- **Lo que sigue:** la pila de Espressif en el P4 en modo "radio por UART"
  (`CONFIG_ZB_RADIO_SPINEL_UART`): su conexión al RCP es código abierto
  (`esp_zigbee_radio_spinel_config_uart`, que le pasa a `esp_radio_spinel` un
  descriptor de archivo); se envuelve para darle un socket UDP local cuyo
  otro extremo va al túnel.

### La pila en el P4: forma su red y se unen equipos (2026-11-24; rama `zigbee-p4`)

En la rama `zigbee-p4`, no en `main` todavía: agranda la imagen del P4 (8 %
libre), lleva contadores de medición (`aos_zigbee_trace.c`, los de
`c6/main/rcp.c`) y hubo cuelgues al arrancar sin causa aclarada (abajo).
**Al final del día:** el P4 corre la pila de Espressif como coordinador con
el C6 de radio, forma su red en el canal 26, vuelve a ella después de
reiniciar, y se le unieron cinco equipos de tres marcas, cada uno
identificado (fabricante y modelo) en segundos, con el Wi-Fi a la misma
velocidad que sin Zigbee. Lo de cada paso:

- **La pila corre en el P4** (`components/aos_154/aos_zigbee_p4.c`,
  `CONFIG_ZB_RADIO_SPINEL_UART`, `CONFIG_OPENTHREAD_SPINEL_ONLY`) con el C6
  de radio: `esp_zigbee_radio_spinel_config_uart` envuelto le da un socket UDP
  local (no bloqueante: su lector lee hasta que no hay más, y uno bloqueante
  se quedaba ahí) cuyo otro extremo va al túnel. **Anda:** reinicia el RCP
  ("Software reset co-processor successfully"), 205 bytes de ida y 292 de
  vuelta, las señales de arranque, sin caídas.
- **La librería de Espressif para el P4 está compilada para el P4 rev 3**:
  `esp_zigbee_task_queue_post` lee el CSR `mintthresh` (0x347), que un P4
  rev 1.3 no tiene (instrucción ilegal). Es la única instrucción así en las
  tres librerías; el módulo entero (seis funciones) está reemplazado por
  `-Wl,--wrap` en `aos_zigbee_taskq.c`, haciendo lo que hace su
  desensamblado. **Avisarle a Espressif.**
- **La RAM interna al arrancar:** con los estáticos de OpenThread y de la
  pila en RAM interna, esp_hosted no consiguió sus buffers de SDIO
  (`assert(buf_mp_g)`, la placa colgada). Van a PSRAM
  (`components/aos_hal/psram.lf`).
- **El almacén:** la pila guarda en el almacén grande de las preferencias
  (`aos_hal_pref_partition()`), no en la `nvs` de la tabla, llena.
- **La red se forma** (2026-11-24, noche, C6 3.0.9-154.12): coordinador en
  el canal 26, `BDB Formation status 0`, y en los arranques siguientes vuelve
  sola a su red (`Device Reboot` OK; pedirle formar otra vez contesta
  `ON_A_NETWORK`, 12, así que no se pide). Manda sus tramas sin errores
  (`frames: N sent, N done` en el log del C6). Las tres cosas que la
  trababan estaban **en el RCP del C6** (`c6/main/rcp.c`), no en la pila:
  1. **El lazo de OpenThread no se despertaba.** Los bytes del P4 entran
     desde una tarea propia (`feeder`), no desde adentro del lazo como los
     de una UART; un pedido que sólo arma un timer (una trama a mandar: su
     espera CSMA es un timer del RCP) quedaba esperando a que otra cosa
     despertara el `select()`, y la pila del P4 se cansaba, reiniciaba el
     RCP (`80 01 02`) y daba `MAC_CHANNEL_ACCESS_FAILURE` (481). Ahora el
     `feeder` llama a `otTaskletsSignalPending()` después de cada pedazo.
  2. **La medida de energía larga no terminaba.** El escaneo que abre la
     formación pide 138 ms (8625 símbolos) de una vez; con el Wi-Fi del C6
     andando el driver quedaba en la medida para siempre, sin su "done" ni su
     "failed" (contado en el C6). El RCP ahora la hace en medidas de 1 ms
     (64 símbolos) hasta cumplir el tiempo, cada una con 20 ms de espera, y
     se queda con la más fuerte, como el escáner de `radio154.c`; además
     define `esp_ieee802154_ed_failed()`, débil y vacía en la IDF (nadie
     avisaba a OpenThread de una medida rechazada). En la primera prueba, 9
     medidas, 7 sin respuesta: el escaneo termina igual.
  3. **La coexistencia Wi-Fi / 802.15.4** no estaba prendida en el RCP
     (`esp_coex_wifi_i154_enable()`, como `zigbee_test.c`), y nadie fijaba
     el turno de 802.15.4 frente al Wi-Fi (OpenThread lo fija por el rol
     sólo cuando arma la red él). El driver tiene uno solo para mandar y
     para escuchar, y un coordinador escucha todo el tiempo: ahora es BAJO
     mientras escucha y ALTO mientras sale una trama (se sube antes de
     mandar y se baja en su "done"/"failed"; medido abajo; primero fue
     MEDIO, y con equipos uniéndose no alcanzó).
  Lo mismo explicaría por qué tampoco formaba con la pila en el C6 (la
  medida larga). **Falta:** que se una un equipo (abrir la red) y sacar los
  contadores de medición (`aos_zigbee_trace.c`, los de `rcp.c`).
- **Wi-Fi, BLE y Zigbee juntos** (2026-11-24, noche; la placa a -20 dBm del
  router, una Mac conectada por BLE como teclado). Cada prueba: 100 pings
  cada 0,2 s y tres bajadas de `/firmware/c6.bin` (1,5 MB) desde el portal;
  del lado del C6, las tramas mandadas y cómo terminaron:

  | Prueba | Ping prom. | Bajada | Tramas Zigbee |
  |---|---|---|---|
  | Sin Zigbee, BLE conectado (3 veces) | 46-59 ms | 340-675 KB/s | - |
  | Zigbee, 802.15.4 en MEDIO, BLE conectado | 68-75 ms | 190-260 KB/s | 1 de 9 rechazada por coex |
  | Igual, con el BLE apagado | 80-83 ms | 180-270 KB/s | 9 de 25 rechazadas |
  | Zigbee, 802.15.4 en BAJO | 37-42 ms | 220-570 KB/s | 16 de 22 rechazadas, una llegó como falla a la pila |
  | Zigbee, BAJO al escuchar y MEDIO al mandar (154.14) | 45-58 ms | 270-570 KB/s | 4 de 13 rechazadas al primer intento, ninguna falla |
  | **Zigbee, BAJO al escuchar y ALTO al mandar** (154.15, con cinco equipos uniéndose) | - | - | 0 de 37 rechazadas |

  - **El BLE no es el problema:** apagarlo no mejoró nada. Lo que le quita
    aire al Wi-Fi es que el coordinador escucha todo el tiempo; con el turno
    repartido así, el Wi-Fi queda como sin Zigbee y las tramas salen (las
    rechazadas se reintentan solas: OpenThread las manda de nuevo).
  - Con poco tráfico Zigbee (sólo los "link status" cada 15 s, sin equipos
    unidos): con equipos reales hay que medir de nuevo.
- **Equipos que se unen** (2026-11-24, noche; `aos_zigbee_devices.c`).
  `POST /api/154/zbpermit {"seconds": N}` abre la red (hasta 254 s); el que
  entra se anuncia, se le piden sus endpoints y después, al Basic del
  primero, fabricante, modelo y alimentación, con el mismo juego de
  atributos que pide Zigbee2MQTT (`0x0004, 0x0000, 0x0001, 0x0005, 0x0007,
  0xFFFE`: algunos Tuya no contestan otra cosa hasta que se les pide ese).
  `GET /api/154` los lista en `zbhost.devices`, y la pantalla de la placa
  avisa cada paso (red abierta/cerrada, entró, quién es, no contestó, se
  fue). Probado con los equipos del usuario, reseteados a mano:

  | Equipo | Fabricante | Modelo | Rol | Señal |
  |---|---|---|---|---|
  | Contacto de puerta | ORVIBO | `e70f96b3773a4c9283c6862dbafb6a99` | a pila | -15 dBm |
  | Sensor de agua | SONOFF | `SNZB-05P` | a pila | -41 dBm |
  | PIR | ORVIBO | `895a2d80097f4ae2b2d40500d5e03dcc` | a pila | -23 dBm |
  | Módulo de 2 relés | `_TZ3000_ruxexjfz` (Tuya) | `TS0002` | router, 220 V, 3 endpoints | -41 dBm |
  | Temperatura y humedad | SONOFF | `SNZB-02D` | a pila | -26 dBm |

  Cada uno quedó identificado en 1 a 3 s. Lo que hubo que arreglar en el
  camino: el centro de confianza ya no exige el intercambio de clave (los
  equipos de antes de Zigbee 3.0 no lo piden y la pila los echaba a los
  14 s, como hace Zigbee2MQTT), el C6 manda en prioridad ALTA frente al
  Wi-Fi (en MEDIO se perdían los beacons que el equipo busca para unirse)
  y los bytes del RCP llegan a la pila en datagramas de 64 (la pila lee su
  "UART" de a 128 y lo que sobraba de uno más largo se perdía: la respuesta
  del Basic llegaba rota). Los equipos del usuario quedaron en la red de
  la placa: hay que volver a emparejarlos con su red de siempre.
- **Lo que mandan** (2026-11-24, noche). Cada equipo que entra se
  configura como lo hace Zigbee2MQTT, un pedido por vez (los "jobs" de
  `aos_zigbee_devices.c`, cada uno a la respuesta del anterior o a los 5 s):
  endpoints, Basic, los clusters de cada endpoint, un *binding* a la placa
  de cada cluster que puede reportar (un Sonoff sólo reporta a lo que está
  atado), la placa como central de alarma de una zona IAS (la dirección
  CIE y el *enroll response*: muchos sensores de puerta, agua o movimiento
  no mandan nada sin eso) y una primera lectura. Lo que llega se lee del
  marco ZCL crudo (el manejador "raw" de la pila, que ve todos los
  clusters): temperatura, humedad, batería, salidas, presencia y zonas IAS
  con nombre, y **todo lo demás crudo** (cluster, atributo o comando,
  valor), para probar cualquier equipo. La app tiene la pestaña **Mi red**:
  armar la red, "Sumar equipos" (254 s con cuenta regresiva) y una tarjeta
  por equipo con sus valores y lo crudo. Probado: el módulo de relés
  (configurado entero, las dos salidas) y el de temperatura (25,9 °C,
  62,1 %, batería 78 %, llegados solos aunque dormido no terminó de
  configurarse). Pulido después de la primera prueba del usuario ("me
  cuesta agregar los equipos"): cinco equipos entraron por el router (el
  módulo de relés) y su anuncio nunca llegó a la placa, que los esperaba
  para listarlos; ahora entra a la lista y se configura apenas la red lo
  autoriza. Ya no se atan los clusters propios de cada marca (0xFC00 y
  más: 5 s perdidos cada uno mientras el equipo a pila se dormía), un
  equipo a medio configurar se configura de nuevo cuando habla, y "Sumar
  equipos" deja la red abierta mientras se está en la pestaña, hasta
  Cerrar. **Falta:** el contacto de puerta, el PIR y el de agua con
  esta versión, mandar órdenes (prender una salida), guardar la lista,
  buscar el modelo en la base de Zigbee2MQTT.
- **La lista se guarda** (2026-11-24, noche, pedido del usuario: "si agregué
  algo y no aparece tendría que estar con un offline"). Cada equipo que entra
  queda en las preferencias (`zbd00`..`zbd31`: quién es, no lo que dijo) y
  vuelve al arrancar, **"sin noticias desde que arrancó la placa"** hasta que
  habla (uno a pila puede tardar una hora); a los de red eléctrica se les
  pregunta enseguida si están, y la tabla de vecinos de la pila suma los que
  la lista no tenga (se mira al formar la red, a los 30 s y a los 2 min: los
  routers aparecen después de su "link status"). **Sacar** (dos toques) le
  pide al equipo que se vaya de la red (Mgmt_Leave) y lo olvida; también
  `POST /api/154/zbremove {"ieee": ...}`.
- **Salidas** (2026-11-24, noche, pedido del usuario): en Mi red, la
  ficha de una salida ("salida 1: prendida") es un botón: tocada, el
  cluster On/Off recibe `Toggle` (lo entiende cualquier interruptor sin
  saber antes su estado) y la placa vuelve a leer el estado (uno que lo
  reporta lo dice igual; uno que no, contesta). También
  `POST /api/154/zbonoff {"ieee": ..., "ep": 1, "on": true}` ("on" sin
  poner: Toggle). Probado con el módulo de relés Tuya TS0002: apagar y
  prender la salida 1 desde el portal. Pulido con el usuario (2026-11-24,
  noche):
  - **On u Off explícito**, según lo que muestra la ficha (Toggle sólo si el
    estado no se sabe): dos Toggle que se cruzan se anulan.
  - **El pedido de la persona primero**: entra a la cola del equipo delante
    de su configuración (después de un reinicio, un toque esperaba detrás
    de todos los pedidos de configuración, 5 s cada uno sin respuesta).
  - **Reintento**: un comando "no entregado" (167, sin acuse) se manda otra
    vez, dos veces a lo sumo. El módulo Tuya TS0002 no toma nada por unos
    segundos justo después de conmutar (guarda el estado en su flash, parece):
    el Off justo después de un On volvía "no entregado".
  - **Los de red eléctrica** (router, o fuente "red" o "continua" del Basic)
    se tratan como despiertos al arrancar: el TS0002 volvía de la lista
    guardada sin esa marca y no se le preguntaba nada (por eso mostraba una
    sola salida).
  - **Lo que falta se vuelve a pedir** cuando el equipo habla (está despierto):
    tipo de zona, batería, nivel, salidas sin estado; a lo sumo una vez por
    minuto.
  - **Fichas más limpias**: las líneas crudas sólo para lo que la placa no
    nombra.
  Probado con el TS0002 desde el portal: ocho órdenes seguidas (On y Off en
  las dos salidas), el estado leído siempre el pedido.
- **Dimmers** (pedido del usuario, para el DomcomDimmer: `../DOMCOM-Dimmer`,
  un *Dimmable Light* 0x0101 router con On/Off y Level Control): cualquier
  equipo con el cluster Level Control (0x0008) muestra "brillo N %" y un
  deslizador; al soltarlo, *Move to Level with On/Off* (0 lo apaga, otro
  nivel lo prende) en 0,5 s, y el nivel se vuelve a leer. También
  `POST /api/154/zblevel {"ieee", "ep", "level": 0-254}`. **Probado con el
  DomcomDimmer** (2026-11-24, noche): entra, se identifica (`DOMCOM` /
  `DOMCOM-Dimmer`, On/Off, Level, su cluster 0xFC00) y obedece nivel y
  encendido, pero con muchas tramas sin acuse de radio (75 de ~280 en el
  log del C6, contra casi ninguna del módulo de relés, a -35 dBm las dos):
  el dimmer recibe mal. Sin confirmar por qué (la antena de su placa C6, el
  ruido del PWM de la tira); los reintentos de la placa lo terminan pasando,
  lento. Dos arreglos en el camino: su primer pedido quedó sin respuesta
  (todavía terminaba de unirse) y su anuncio 40 s después no reinició la
  configuración (una espera de un minuto entre configuraciones, sacada: una
  configuración está "en curso" sólo mientras tiene pedidos pendientes); y
  una entrada fantasma `ffffffffffffffff` (una fila de la tabla de vecinos
  sin dirección larga todavía) que ya no entra, ni a la lista guardada.
- **Más equipos, aunque no se puedan probar acá** (2026-11-24, noche, pedido
  del usuario; dos subagentes leyeron las fuentes de Zigbee2MQTT para lo de
  Tuya). **Sin probar en la placa salvo que diga lo contrario.**
  - **Una tabla de valores** (`components/aos_154/aos_zigbee_fmt.c`, para la
    pantalla y el portal): luz (lux, 0x0400), presión (0x0403), CO2
    (0x040D), PM2.5 (0x042A), humedad del suelo (0x0408), caudal (0x0404),
    energía y consumo (Metering 0x0702, con su multiplicador y divisor),
    tensión, corriente y potencia (0x0B04, ídem), posición de cortina
    (0x0102), termostato (0x0201: ambiente, consigna, modo), temperatura de
    color (0x0300), cerradura (0x0101), ventilador (0x0202), Analog y
    Multistate Input. Se leen al configurar y se nombran con su unidad.
  - **Controles**, por un camino genérico (cualquier comando del cluster o
    escritura de atributo, con reintento y lectura de vuelta;
    `POST /api/154/zbcmd` y `zbwrite`): cortina (subir, parar, bajar y un
    deslizador de posición), termostato (consigna ±0,5 °C), luz de color (un
    deslizador de temperatura de color), cerradura (cerrar con llave, abrir),
    ventilador (el modo siguiente).
  - **Botones y controles remotos**: los comandos que manda el equipo (On/Off,
    Level, escenas, el 0xFD de los TS004x de Tuya, el Multistate de los
    botones tipo Aqara) se muestran como "botón N: doble" con aviso en
    pantalla; para eso se atan también los clusters de salida del equipo.
  - **Tuya 0xEF00** (casi todo lo barato de Tuya: sensores, válvulas,
    cortinas, presencia): los datapoints se decodifican genéricamente (dp,
    tipo, valor: `dp | tipo | largo BE | valor BE`, varios por trama), un dp
    booleano se cambia tocándolo, cualquiera por `POST /api/154/zbdp`; la
    placa contesta la hora (0x24, UTC y local, desde 1970) y el estado de
    conexión (0x25), y pide todos los datapoints al configurar (dataQuery).
    Qué significa cada dp es de cada equipo (Zigbee2MQTT lo tiene por
    modelo): por ahora se ven los números.
  - **Emisor IR de Tuya** ("Zosung": ZS06, TS1201): aprender un código del
    control remoto (`{"study":0}` por 0xE004, la transferencia por 0xED00 en
    pedazos con suma de bytes, el código en base64 como lo muestra
    Zigbee2MQTT) y mandarlo (el JSON `{"key_num":1,...,"key_code":...}` en
    pedazos de 50 que el emisor va pidiendo, el primer marco de nuevo cada
    2 s hasta que conteste). En Mi red: "Aprender un código" y "Enviar el
    último aprendido"; `POST /api/154/zbir`. El usuario tiene uno
    (`_TZ3290_nba3knpsarkawgnt`, TS1201). **Anda** (2026-11-24, noche): el
    usuario aprendió un código del control remoto de su televisor (73
    bytes) y manejó el volumen con "Enviar el último aprendido". Para
    unirse necesitó la apertura "Zigbee 3.0" (fila 15 de la bitácora).
    El primer envío después de aprender a veces tarda: el emisor recién
    contestó al tercer o cuarto primer marco (7 s) y una vez a ninguno
    en 10 s; los siguientes salieron en medio segundo. Por eso la espera
    es de 15 s, y cada marco de la transferencia queda en el log
    ("IR: from the blaster") para mirar el próximo que falle.
  - **Códigos guardados** (2026-11-25): "Guardarlo..." le pone nombre al
    último aprendido y queda en `zigbee/ir.txt` de la tarjeta (una línea
    por código: IEEE del emisor, nombre, el código tal cual lo dio). En la
    tarjeta del emisor, un botón por código: tocarlo lo manda, mantenerlo
    dos veces lo borra. Portal: `zbir` con `save`, `saved` (mandar),
    `forget`, y `raw` (duraciones cualesquiera); la lista en
    `zbhost.ir.codes`.
    **En la placa (2026-11-25):** aprender, guardar y mandar andan; cada
    envío sale entero en ~1 s (doce pedazos de 50 que pide el emisor, el
    primer marco contestado al toque). Apareció un **eco**: un cuarto de
    segundo después de mandar, el TS1201 ofreció como "aprendido" un
    código de 371 bytes que nadie pidió (se oyó a sí mismo, todavía
    escuchando), y pisó al aprendido de verdad (197 bytes), que fue el que
    se guardó. Desde entonces la placa toma un código aprendido sólo
    mientras ella pidió aprender, y si llega otro le repite al emisor que
    deje de escuchar.
  - **El formato del código** (`aos_zigbee_ir.c`, también en el
    simulador): base64 de FastLZ nivel 1 sobre duraciones de 16 bits LE
    en µs, empezando por una marca. La placa lo lee y lo arma (copias
    hacia atrás de 3 bytes o más; un aire acondicionado queda en una
    fracción, y el emisor lo pide de a 50 bytes), así manda cualquier
    código en duraciones: la biblioteca SmartIR de la app Infrarrojo, que
    las apps alcanzan por `aos_zigbee_ir_*` (exportadas a las apps, sólo
    esas, con `aos_zigbee_info` y `aos_zigbee_devices`:
    `tools/gen_symbols.py`). Probado ida y vuelta (200 códigos al azar) y
    contra el lector de la página de Infrarrojo.
  - **Simulador:** `P4_SIM_ZB_IR=1` finge la red en el canal 26 con un
    TS1201 y un sensor Tuya TS0601 (`_TZE200_ysm4dsb1`, datapoints de
    temperatura, humedad y batería): aprender da un NEC (dirección 0x04)
    a los 3 s, mandar termina en 0,8 s y queda en el log.
- **Nombre propio** (2026-11-25): tocar el título de una tarjeta le pone
  al equipo un nombre del usuario ("Sensor de la cocina"); va en la
  preferencia del equipo (`zbdNN`, un tercer `|` después del modelo) y lo
  ven la pantalla, el portal (`name`, `POST /api/154/zbname`) y la lista
  de emisores IR.
- **Qué es cada equipo Tuya y qué dice** (2026-11-25): la tarjeta lee
  `zigbee/tuya.txt` (parte del sistema `sys-zigbee-tuya` de la Tienda,
  asset `zigbee_tuya.txt`, 190 KB, no va a git), armado por
  `tools/zigbee/gen_tuya.py` desde `src/devices/tuya.ts` de
  zigbee-herdsman-converters (MIT): 1424 pares (modelo Zigbee,
  fabricante) con marca, modelo y descripción, y 313 juegos de
  datapoints con nombre, tipo y escala. Así un TS0601 dice "Tuya
  RSH-HS06, Temperature and humidity sensor" y "temperatura 21.5 °C" en
  vez de "dp 1: 215". Los nombres más comunes van en castellano
  (`aos_zigbee_tuya.c`), el resto como los tiene Zigbee2MQTT. Lo que el
  script no entiende (horarios de termostato, umbrales en bloques) queda
  como número. Un equipo se busca una vez (dos pasadas al archivo) y
  queda en PSRAM. Sólo `tuya.ts`: los Tuya que Zigbee2MQTT define en
  otros archivos (el TS1201, los de Moes con nombre propio) no están.
  **Sin probar con un equipo real**: ninguno de los del usuario es TS0601
  (el TS0002 sí está, y su tarjeta ya dice "Tuya TS0002_basic, 2 gang
  switch module" si el pack está en la tarjeta).
  Para la próxima release: `python3 tools/zigbee/gen_tuya.py
  <zigbee-herdsman-converters>/src/devices/tuya.ts
  build/zigbee/zigbee_tuya.txt` y subirlo como `zigbee_oui.bin`
  (ya está en `CARRIED_PACKS`).
  - **"Sumar Zigbee 3.0"** en Mi red (`POST /api/154/zbpermit` con
    `board`, `strict` y `static`): abre la red sólo en la placa (no en los
    routers: cierra todo y abre la placa 1,5 s después, si no un "cerrar"
    a todos que llega tarde la cierra), exige el intercambio de clave de
    Zigbee 3.0 y ofrece sólo el Request Key clásico. Para el equipo que
    titila y nunca termina de sumarse con el botón común. Los anteriores a
    Zigbee 3.0 (los ORVIBO) no entran así: el botón común es el suyo.
  - **Lo que no terminó de configurarse se vuelve a pedir:** el pedido de
    endpoints con tres intentos, y cada minuto se rehace la configuración
    de un equipo despierto que quedó a medias.
- **Nota para probar desde la Mac:** zsh no parte `$var` en palabras: un
  `for p in "1 true"; set -- $p` armó JSON roto y la placa contestó
  `ok:false` (con razón). El portal estaba bien.
- **Para qué** (el usuario, 2026-11-24): un banco de pruebas de campo. Si
  un equipo en una obra no se une al Home Assistant o anda raro, se lo une
  a la placa y se ve qué es, qué manda y si sus salidas responden; y al
  revés, que la placa **emule** un equipo así (un sensor, un relé) para ver
  si el coordinador de Home Assistant lo lee o lo maneja bien. Por eso
  nada está atado a los equipos de la prueba: cualquier cluster se muestra.
- **Cuelgues al arrancar (aclarado al final del día: fila 14 de la bitácora):** cuatro veces en el día, siempre en el primer
  arranque después de instalar por OTA un firmware de esta rama, la placa
  quedó colgada (la pantalla celeste del arranque, o negra, sin red ni USB);
  un reset la trae, y arranca bien. Sin volcado, sin causa aclarada: no
  siempre pasa (de unas quince instalaciones del día), y las secciones de
  RAM interna eran iguales entre una imagen que colgó y una que no. Una vez
  se vio que esp_hosted no conseguía sus buffers de SDIO (eso se arregló
  mandando estáticos a PSRAM, arriba), pero volvió a pasar después. **Para
  la próxima:** dejar `tools/console_log.py` grabando antes de instalar, para
  ver dónde se queda.

### Bitácora del 2026-11-24: las vueltas que dio Zigbee

Un día entero, de la mañana a la noche, para pasar de "escuchar" a
"coordinar una red". En orden, lo que trabó y cómo se destrabó, para que
quien siga no repita las vueltas:

| # | Qué pasó | Por qué | Cómo quedó |
|---|---|---|---|
| 1 | La pila Zigbee de Espressif no cabía en el C6: la actualización se cortaba a 1 572 200 bytes | las ranuras de fábrica del C6 son de 1,5 MB (tiene 4 MB, 960 KB sin usar) | el C6 agranda sus ranuras a 1,94 MB a pedido, sin cable (`c6/main/slots.c`, `docs/C6.md` "The slots") |
| 2 | Con la pila adentro, el C6 se quedó sin memoria al arrancarla (con `-O2`) o corrió sin formar la red (con `-Os`, 78 KB libres) | el C6 comparte su RAM con el Wi-Fi y el BLE | la pila va al P4 y el C6 queda de radio (RCP), como pide el usuario |
| 3 | No hay UART entre el P4 y el C6, y esp_hosted no implementa "spinel por su transporte" | - | túnel propio: el spinel del RCP por peer_data (`rcp.c` envuelve `otPlatUartSend`), y en el P4 un par de sockets UDP locales en lugar del UART que espera la pila |
| 4 | El lector de la pila se quedaba colgado en el socket | lee "hasta que no hay más", y un socket bloqueante espera para siempre | el socket es `O_NONBLOCK`, como un UART abierto así |
| 5 | Instrucción ilegal en `esp_zigbee_task_queue_post` | la librería para el P4 está compilada para el rev 3 y lee el CSR 0x347 (`mintthresh`), que el rev 1.3 no tiene | el módulo de la cola de tareas reemplazado entero por `--wrap` (`aos_zigbee_taskq.c`); **avisarle a Espressif** |
| 6 | La placa colgada al arrancar, `assert(buf_mp_g)` en el SDIO | los estáticos de OpenThread y de la pila se comían la RAM interna que esp_hosted necesita | van a PSRAM (`psram.lf`) |
| 7 | La pila no tenía dónde guardar su red | la `nvs` de la tabla está llena de preferencias | usa el almacén grande de las preferencias |
| 8 | **La formación quedaba "en curso" para siempre**, sin nada al radio (igual que en el C6) | el escaneo de energía de 138 ms nunca terminaba en el C6 con su Wi-Fi andando, y el driver no decía ni "listo" ni "falló" (`esp_ieee802154_ed_failed` es débil y vacía) | el RCP hace la medida en pedazos de 1 ms con espera, como el escáner (`rcp.c`) |
| 9 | Formada la red, cada trama tardaba 6 s, fallaba (481) y la pila reiniciaba el RCP | el lazo de OpenThread del C6 no se despertaba al llegar un pedido desde el túnel, y el timer del CSMA no se revisaba | el `feeder` despierta el lazo (`otTaskletsSignalPending`) |
| 10 | Con Zigbee, el Wi-Fi a la mitad (250 contra 500 KB/s) | el turno de 802.15.4 en MEDIO todo el tiempo, y un coordinador escucha siempre | BAJO al escuchar, ALTO al mandar; el BLE no tenía que ver (medido apagándolo) |
| 11 | El primer sensor entró y a los 14 s lo echaron ("authorization timeout") | la pila exigía el intercambio de clave de Zigbee 3.0, que los equipos anteriores no hacen | no se exige, como en Zigbee2MQTT |
| 12 | El sensor no encontraba la red | el Wi-Fi rechazaba 4 de 5 beacons que le contestaban (prioridad MEDIO) | prioridad ALTA al mandar: 0 rechazos |
| 13 | El fabricante y el modelo llegaban rotos ("dropping radio frame: Parse") | la pila lee su "UART" de a 128 bytes y, de un datagrama más largo, el resto se pierde | el túnel le entrega datagramas de 64 bytes |
| 14 | La placa "colgada" en el primer arranque después de un OTA (cinco veces en el día): sin tarjeta y sin Wi-Fi | la microSD y el C6 comparten el único controlador SDMMC del P4; primero pareció un cruce al arrancar (y se cambió el orden: la tarjeta y después el enlace del C6, que quedó, y la pantalla sale antes), pero el puerto serie mostró la causa: 8 s después de un arranque bueno, con la tarjeta escribiendo (el primer arranque de un firmware nuevo escribe el caché de las apps y un certificado) y el Wi-Fi levantando, el controlador se colgó, esp_hosted reinició la placa, y la tarjeta quedó trabada (un reinicio por software no le corta la corriente) | la tarjeta a 20 MHz en vez de 40, como el ejemplo de Espressif de este mismo par (`mcu_hosted_sdio_sdmmc_combined`, que cita esp-idf #16233); cinco rondas bajo carga sin error. **No era de Zigbee: en `main`** (`e701299d`). Hay que seguir mirando: si vuelve, el puerto serie grabando |
| 15 | El emisor IR (TS1201) se unía, titilaba y nunca terminaba: la placa lo veía anunciarse y a los segundos desaparecía | es Zigbee 3.0 de Tuya: espera el intercambio de la clave del centro de confianza (que la placa no exige, fila 11) y, al abrir la red en todos los routers, a veces se colgaba de uno (el relé) que no lo terminaba | "Sumar Zigbee 3.0": abre sólo la placa, exige el intercambio y ofrece el Request Key clásico; entró a la primera. El usuario aprendió un código de su televisor y le manejó el volumen. **No reiniciar la placa mientras un equipo se está configurando** (pasó una vez, y hubo que unirlo de nuevo) |

Y lo que lo hizo visible: contadores en cada llamada entre la pila y su
plataforma (`aos_zigbee_trace.c`), el volcado de los bytes de spinel en cada
dirección (decodificados a mano: así se vio el pedido de escaneo aceptado y
sin resultado, y la trama sin respuesta seguida del reset), el log del C6
traído al P4, y el puerto serie grabando.
