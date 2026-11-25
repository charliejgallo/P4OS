# P4OS — La placa emula un equipo Zigbee

**Dónde quedó (2026-11-25):** las etapas 0 y 1 están hechas: la placa se
unió al Zigbee2MQTT del usuario como equipo, la luz y los valores van y
vienen, un reinicio vuelve solo y Volver a Mi red trae la red propia
intacta (abajo, "Etapa 1 en la placa"). Lo que hay: `aos_zigbee_emu.c` (el equipo
"luz y sensor", la búsqueda de red cada 5 s, el registro en palabras de lo
que pide el coordinador, salir y volver), `aos_zigbee_nvs.c` (el desvío de
`"zigbee"` a `"zbemu"`, confirmado en el mapa del enlazador: la librería lo
llama), `aos_zigbee_p4.c` (router o coordinador según el modo,
`max_children` en la preferencia `zbe_children`, 0 por defecto), el
arranque después del reinicio (`aos_zigbee_boot` en `main.c`, la
preferencia `zbe_boot` de una vez), la pestaña Emular, `POST
/api/154/zbemu` y su sección en la página Zigbee del portal, y el
coordinador falso del simulador (`P4_SIM_ZB_EMU=1`). Quedó afuera de la
etapa 0: la lista de redes abiertas (`aos_zigbee_emu_scan`; se une a la
primera que encuentra), "Pausar" (Volver a Mi red guarda la otra red) y
mostrar la configuración de reportes al lado de cada valor (está en el
registro). Más abajo, el diseño como se escribió.

**Etapa 1 en la placa (2026-11-25, con el usuario), contra su Zigbee2MQTT
con un Sonoff ZBDongle-E (adaptador `ember`):**

- "Reiniciar y emular": la pila arrancó como router a los 10 s del
  arranque y buscó cada ~10 s (cada búsqueda en los 16 canales tarda ~5
  s; antes de abrir la red, "Comm failed in Join"). Abierta la red en
  Zigbee2MQTT, **entró al quinto intento** (canal 11).
- **La entrevista entera**, en el registro, en 3 s: descriptor del nodo,
  endpoints 1, 2 y 242 (Green Power), Basic de a un atributo (0005 0004,
  0007, 0000-0003, 0006, 4000), y en orden: ató On/Off y configuró su
  reporte (0 s a 65000 s), Power Configuration (batería, 3600 a 65000 s),
  temperatura y humedad (10 s a 3600 s), leyendo cada uno después.
- **Sin el intercambio de clave Zigbee 3.0** (`TCLK_READY` no llegó) y
  igual quedó en la red: ember no lo exige, como Zigbee2MQTT con los
  equipos viejos.
- **Zigbee2MQTT lo expone** sin definición propia: Identify, State
  (On/Off), Power-on behavior, Battery, Temperature y Humidity del
  endpoint 2, Linkquality 255 (`docs/img/zigbee-emular.jpg`, con las
  direcciones borroneadas).
- **Anda todo lo de la luz y los valores:** Prender desde Zigbee2MQTT
  llega a la placa ("mandó Prender a la luz", "la luz quedó prendida") y
  la placa lo muestra; los deslizadores (29 °C, 17 %, 35 %) y "Reportar
  ahora" aparecen en Zigbee2MQTT.
- Zigbee2MQTT pregunta la dirección larga cada tanto (su chequeo de que
  el equipo sigue vivo).
- **La luz en los dos sentidos, confirmado por el usuario:** tocarla en la
  placa cambia el State en Zigbee2MQTT, y el State de Zigbee2MQTT la
  cambia en la placa.
- **Un reinicio vuelve solo:** con la red guardada en `"zbemu"`, "Unirse"
  volvió a la red sin que la abrieran (`DEVICE_REBOOT` 0, la misma
  dirección corta), y un reinicio (`/api/restart`) también. Para eso
  hizo falta `zbe_mode` (queda puesto mientras se emula): `zbe_boot` sola
  se borra al usarse y la pila no arrancaba.
- **Volver a Mi red, lo más importante: anda.** Se reinició, arrancó
  como coordinador en el canal 26 y la pila encontró su red en
  `"zigbee"` ("back on its network", `DEVICE_REBOOT` 0, no una red
  nueva): los 7 equipos en la lista, y en un minuto contestaron el
  TS0002, el dimmer, el emisor IR y el PIR. Las dos redes quedaron cada
  una en su namespace, sin pisarse.

Quedan para la etapa 2: los otros tipos (puerta, relé, dimmer, botón),
probar contra ZHA, y elegir la red cuando hay más de una abierta.

Diseño, escrito antes del código (2026-11-25). Sigue a `ZIGBEE.md` (la pila de
Espressif en el P4 con el C6 de radio, la red propia de la placa como
coordinador) y es el punto 3 de "La idea" de ese documento.

## Para qué

El banco de pruebas de campo (el usuario, 2026-11-24): si en una obra un
equipo no se une al coordinador de Home Assistant (ZHA o Zigbee2MQTT) o anda
raro, se lo une a la placa y se ve qué es (eso ya anda: "Mi red"). Al revés,
que es lo de acá: **la placa se une a la red de Home Assistant haciéndose
pasar por un equipo** (un sensor de temperatura y humedad, uno de puerta, un
relé o una luz, un dimmer, un botón) para ver si el coordinador lo lee y lo
maneja bien, y **qué le pide** al unirlo. El usuario tiene un coordinador de
Home Assistant para probar.

Sólo en redes del usuario o de quien lo contrató: la placa entra sólo a una
red que alguien abrió desde su Home Assistant.

Las marcas de este documento: **(encabezados)** es lo leído en
`managed_components/espressif__esp-zigbee-lib` 2.1.0; **(medido)** es de la
placa; **(de memoria)** es lo que sé de ZHA y Zigbee2MQTT sin haberlo
mirado ahora en sus fuentes: hay que confirmarlo con el coordinador del
usuario.

## 1. ¿La pila del P4 puede unirse a una red ajena?

**Sí, como router. Como dispositivo final (ZED), no.**

- **Lo que hay para el P4 (encabezados):** `lib/esp32p4` trae sólo
  `libesp-zigbee-core.zczr.*.a` (coordinador o router) y
  `libesp-zigbee-idf.remote.*.a` (radio por spinel). **No hay `.zed` para el
  P4** (ni para ningún chip con la radio afuera: los `.zed` son de C5, C6,
  H2, H21, H4 y S31). `CMakeLists.txt` de la librería elige uno u otro por
  `CONFIG_ZB_ZCZR`/`CONFIG_ZB_ZED`; nuestro `sdkconfig.defaults` tiene
  `CONFIG_ZB_ZCZR=y`. O sea: el mismo firmware de hoy sirve para coordinador
  y para router, sin compilar nada distinto.
- **Qué significa:** la placa se presenta como un equipo "de red eléctrica"
  que enruta (como un enchufe o una lámpara). Para una luz, un relé o un
  dimmer es lo natural. Un sensor emulado también se presenta como router:
  Home Assistant lo va a mostrar como "Router, red eléctrica", no "a pila".
  Sirve para probar si lee la temperatura, la puerta o el botón, pero **no
  prueba lo propio de los equipos dormidos** (que despiertan cada tanto, el
  *poll*, los tiempos de "no disponible" de 25 h, el cambio de padre).
- **El rol (encabezados):** `esp_zigbee_config_t.device_config.device_type =
  EZB_NWK_DEVICE_TYPE_ROUTER` en `esp_zigbee_init()` (`esp_zigbee.h`), con
  `zczr_config.max_children`. También `ezb_nwk_set_device_type()` (`nwk.h`),
  que contesta `EZB_ERR_INV_STATE` si ya está en una red: el rol se fija
  antes de arrancar.
- **Unirse (encabezados, `bdb.h`):** como el coordinador de hoy
  (`aos_zigbee_p4.c`, `on_signal`), pero con *steering* en vez de formación:
  `EZB_ZDO_SIGNAL_SKIP_STARTUP` → `ezb_bdb_start_top_level_commissioning(
  EZB_BDB_MODE_INITIALIZATION)`; en `EZB_BDB_SIGNAL_DEVICE_FIRST_START` (o
  `DEVICE_REBOOT` con error) → `EZB_BDB_MODE_NETWORK_STEERING`; el resultado
  llega en `EZB_BDB_SIGNAL_STEERING` (estado 0: adentro). Los canales:
  `ezb_bdb_set_primary_channel_set(0x07FFF800)` (los 16). Cancelar:
  `ezb_bdb_cancel_steering()`. Es lo que hace el ejemplo `HA_on_off_light`
  de Espressif (de memoria).
- **Elegir la red** cuando hay más de una abierta: `ezb_nwk_scan()` con
  `EZB_NWK_SCAN_TYPE_ACTIVE` (`nwk.h`) da cada beacon con PAN, PAN extendido,
  canal, RSSI y `permit_join`; y `ezb_aps_set_use_extended_panid()` (`aps.h`)
  restringe el *steering* a ese PAN extendido.
- **Las claves (encabezados y la librería):** la clave global de enlace por
  defecto es la de siempre, `ZigBeeAlliance09` (está en
  `libesp-zigbee-core.zczr.release.a`, junto con `ZigBeeAlliance18`); se
  puede fijar con `ezb_secur_set_global_link_key()`. Para el intercambio de
  la clave del centro de confianza que piden los coordinadores Zigbee 3.0,
  `ezb_secur_set_tclk_exchange_required(true)` (`secur.h`: "router/end
  device without required will not request key"): **al revés que en el
  coordinador de hoy, que lo pone en `false`**. Llega
  `EZB_SECUR_SIGNAL_TCLK_READY` cuando terminó. Código de instalación (del
  lado del que se une): `install_code_policy = true` en la configuración y
  `ezb_secur_ic_set()`; en Home Assistant se carga el código del otro lado.
  No hace falta para empezar.
- **Volver después de reiniciar:** igual que el coordinador, la red queda en
  el NVS y vuelve sola (`EZB_BDB_SIGNAL_DEVICE_REBOOT` con estado 0). Un
  *rejoin* forzado al arrancar: `ezb_bdb_set_router_rejoin_required(true)`.
- **Salir:** `ezb_bdb_reset_via_local_action()` (`bdb.h`): avisa que se va,
  borra los datos de esa red salvo el contador de tramas, y da
  `EZB_ZDO_SIGNAL_LEAVE` con `EZB_ZDO_LEAVE_TYPE_RESET`. Si es Home
  Assistant el que la saca ("Quitar dispositivo" manda Mgmt_Leave), llega la
  misma señal.
- **No adoptar hijos ajenos:** como router, otros equipos de la obra podrían
  elegir a la placa de padre, y al irse ella tendrían que buscar otro. Con
  `max_children = 0` (o `ezb_nwk_set_max_children(0)`) no acepta ninguno;
  igual puede quedar en rutas del mesh, que se rehacen solas. **A medir:**
  que con 0 siga enrutando y que el *steering* no lo rechace.

### Un solo NVS, dos redes

La pila guarda su red en un único *namespace* NVS, **`"zigbee"`**, dentro de
la partición que se le da (`storage_partition_name`; hoy `prefs`, el almacén
grande de las preferencias). Lo saqué de la librería: el string `zigbee`
está junto a los formatos de clave `ZB%02x`/`ZB%02x%02x` en
`esp_zigbee_plat_datasets.c.obj`, que llama a `nvs_open_from_partition` (un
símbolo sin resolver: lo resuelve el enlazador). Si la placa se une a Home
Assistant con ese mismo *namespace*, **pisa la red propia** (la clave, el
PAN, la tabla de hijos).

Tres caminos:

| Camino | Cómo | Contra |
|---|---|---|
| **A. Otro namespace (el elegido)** | `-Wl,--wrap=nvs_open_from_partition`: si piden `"zigbee"` y se está emulando, se abre `"zbemu"`; cualquier otro pedido pasa igual | ninguno visible: es el mismo truco que `aos_zigbee_taskq.c`; las llamadas propias de P4OS (`aos_hal_p4.c`) pasan derecho |
| B. Copiar y restaurar | antes de emular, copiar las claves `ZB*` de `"zigbee"` a `"zbhome"`, borrar; al volver, al revés | un corte a mitad pierde la red propia |
| C. Otra partición | `storage_partition_name` distinto | la `nvs` está llena y cambiar la tabla pide el cable |

Con A, cada red tiene lo suyo y `ezb_plat_datasets_wipe()` o
`esp_zigbee_factory_reset()` en modo emulación borran sólo `"zbemu"`.
**Ninguno de los dos namespaces entra en los respaldos:** el respaldo
recorre sólo `"p4os"` (`aos_hal_pref_foreach`, `PREF_LIST` en
`aos_hal_p4.c`). Está bien así: **la clave de la red del cliente queda en la
placa** mientras no se salga de esa red, y no debe viajar en un respaldo.

### Cambiar de rol

- La pila arranca a pedido (`aos_zigbee_start`, desde "Mi red" o
  `POST /api/154/zbhost`), no al encender. **Si en este arranque todavía no
  corrió, arranca directo como router**: nada que reiniciar.
- Si ya corre como coordinador, hay que pararla. Existen
  `esp_zigbee_stop()` (sale del lazo) y `esp_zigbee_deinit()`
  (`esp_zigbee.h`), pero **no sé si vuelve a arrancar limpia en el mismo
  encendido** (el enlace de spinel, los eventfd, la cola envuelta, los
  endpoints ya registrados con `ezb_af_device_desc_register`, que no tiene
  contrario). Etapa 1: el cambio de rol **reinicia la placa** (lo pide la
  persona desde la app, con aviso) y arranca en el modo pedido. Probar
  stop/deinit/init más adelante.
- **Los endpoints se registran antes de `esp_zigbee_start()`**, como el
  `custom_gateway` de hoy: cambiar de "qué equipo es" también pide
  rearrancar la pila. Por eso la etapa 1 es un solo equipo compuesto.

### Qué pasa con la red propia mientras se emula

Queda apagada: sus equipos siguen entre ellos (los routers como el TS0002
mantienen el mesh), los de pila reintentan, nadie nuevo entra y la clave no
cambia. La lista de "Mi red" (`zbd00`..`zbd31` en las preferencias) no se
toca. Al volver, la pila arranca con `"zigbee"`, vuelve a su red
(`DEVICE_REBOOT` 0, mismo canal, PAN y clave) y los equipos siguen; los de
pila aparecen cuando hablan, como después de cualquier reinicio
(`ZIGBEE.md`, "La lista se guarda").

**Cuidado:** en modo emulación no hay que llamar a `aos_zigbee_devices_init()`.
Su manejador crudo (`on_raw` en `aos_zigbee_devices.c`) suma a la lista
cualquier dirección corta que le hable: metería al coordinador de Home
Assistant como equipo de "Mi red" y lo guardaría. Tampoco el endpoint
`custom_gateway` (fabricante `P4OS`, modelo `hub`).

## 2. Cómo se declara el equipo emulado

### Lo común a todos

- **Perfil Home Automation 0x0104**, un endpoint por función (1, 2, ...).
  Las funciones `ezb_zha_create_*` (`zha.h`) arman el endpoint entero:
  `on_off_light` (0x0100), `dimmable_light` (0x0101), `mains_power_outlet`
  (0x0009), `on_off_switch` (0x0000), `temperature_sensor` (0x0302),
  `light_sensor`, `thermostat`, `window_covering`, `door_lock`... Un
  cluster que no traen se suma con `ezb_af_endpoint_add_cluster_desc()` y
  su `ezb_zcl_<cluster>_create_cluster_desc()`; el device ID se cambia con
  `ezb_af_ep_desc_set_app_device_id()` (`af.h`).
- **Basic (0x0000):** fabricante `P4OS` y un modelo por tipo
  (`P4OS-EMU-LT`, `P4OS-EMU-TH`...) con `ezb_zcl_basic_cluster_desc_add_attr`,
  como el gateway de hoy; además `PowerSource` 0x01 (red, que es la verdad
  para un router), `SWBuildID` con la versión de P4OS y `DateCode`. Son los
  atributos que lee Zigbee2MQTT en su entrevista (los mismos de
  `aos_zigbee_devices.c`: 0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE).
  **No usar huellas de equipos reales** por defecto (abajo, etapa 4).
- **Identify (0x0003):** lo traen todos los `EZB_ZHA_*_CONFIG()`. Home
  Assistant lo usa ("Identificar"): la placa lo muestra en pantalla
  (`EZB_ZCL_CORE_IDENTIFY_EFFECT_CB_ID` y el atributo IdentifyTime).
- **Los valores:** `ezb_zcl_set_attr_value(ep, cluster, EZB_ZCL_CLUSTER_SERVER,
  attr, EZB_ZCL_STD_MANUF_CODE, &v, false)` (`zcl_common.h`), siempre desde
  la tarea de la pila (`esp_zigbee_task_queue_post`).
- **Los reportes:** el coordinador ata (ZDO Bind_req) cada cluster a sí
  mismo y configura reportes (Configure Reporting, comando general 0x06).
  La pila contesta las dos cosas sola, guarda la configuración (dataset
  `EZB_DATASETS_KEY_ZCL_REPORTING_INFO`) y reporta cuando el valor cambia
  más que el mínimo pedido, o al vencer el máximo (`zcl_reporting.h`; el
  mínimo por defecto es 5 s, `EZB_ZCL_MIN_REPORTING_INTERVAL_DEFAULT`).
  Para leer lo configurado: `ezb_zcl_reporting_info_find()`. Para mandar
  ya, sin esperar: `ezb_zcl_report_attr_cmd_req()` (`zcl_general_cmd.h`).
- **Lo que manda la placa por su cuenta** (un botón, la puerta): con
  dirección `EZB_ADDR_MODE_NONE` sale por la tabla de *bindings* (lo que
  ató el coordinador); si no hay ninguno, al coordinador (0x0000), endpoint
  1. Con `ezb_zcl_custom_cmd_req()` (`custom.h`) y `fc.direction` como haga
  falta, igual que `aos_zigbee_devices.c` (`J_ZCL`).
- **Lo que pide el coordinador**, para el registro (abajo): todo marco ZCL
  pasa por `ezb_zcl_raw_command_handler_register()` antes que la pila (se
  devuelve `false` para que siga); los pedidos ZDO (Node_Desc 0x0002,
  Simple_Desc 0x0004, Active_EP 0x0005, Bind 0x0021, Mgmt_Lqi 0x0031,
  Mgmt_Leave 0x0034) por `ezb_apsde_data_indication_handler_register()`
  (`aps.h`, perfil 0x0000; también `false`). Los cambios que hizo el
  coordinador (prendió, cambió el nivel) llegan por
  `ezb_zcl_core_action_handler_register()` con
  `EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID` (`zcl_core.h`). Nada de eso usa hoy el
  coordinador, salvo el crudo (que no se registra al emular). **No**
  registrar el de confirmaciones (`ezb_apsde_data_confirm_handler_register`):
  según `aps.h`, entonces la aplicación se hace cargo de todas.

### Por tipo

| Tipo | Device ID | Clusters servidor (además de Basic e Identify) | Notas |
|---|---|---|---|
| Luz / relé | 0x0100 luz, 0x0009 enchufe | Groups, Scenes, **On/Off 0x0006** | la pila atiende On/Off/Toggle sola y avisa el cambio; ZHA decide "luz" o "interruptor" por el device ID (de memoria) |
| Dimmer | 0x0101 | ídem + **Level 0x0008** | Move to Level con transición: la pila mueve el nivel (a medir cómo avisa los pasos) |
| Temperatura y humedad | 0x0302 | **0x0402**, **0x0405**, **Power Config 0x0001** (BatteryPercentageRemaining 0x0021 en medios %, BatteryVoltage 0x0020 en 100 mV) | valores en 0,01 °C y 0,01 %; la batería es de mentira (un router no tiene), útil para ver si Home Assistant la muestra |
| Puerta | 0x0402 (IAS Zone) | **IAS Zone 0x0500**, ZoneType 0x0015 | inscripción, abajo |
| Botón | 0x0000 (o 0x0103) | (Basic, Identify) y **On/Off cliente** | manda On/Off/Toggle a lo atado |

**La inscripción IAS** (puerta, agua, movimiento): el coordinador escribe su
dirección en `IAS_CIE_Address` (0x0010) y manda Zone Enroll Response; la
zona queda "inscripta" (`ZoneState` 1) con su `ZoneId`. Algunos esperan
antes un Zone Enroll Request del equipo: `ezb_zcl_ias_zone_enroll_cmd_req()`
(`ias_zone.h`), mandado al ver escrita la dirección si en 2 s no llegó la
respuesta. El cambio de estado es el comando 0x00 del servidor al cliente
(Zone Status Change Notification: estado de 16 bits, estado extendido,
ZoneId, demora de 16 bits) a la dirección del CIE. `ias_zone.h` no trae una
función para mandarlo, pero la librería tiene
`ias_zone_cluster_srv_set_attr_val_post_proc`: **probablemente** la pila lo
manda sola al cambiar `ZoneStatus` con `ezb_zcl_set_attr_value`. A medir; si
no, `ezb_zcl_custom_cmd_req` con `direction = 1`.

### Qué hacen Home Assistant con un modelo que no conoce (de memoria)

- **Zigbee2MQTT:** lo entrevista igual (Basic, node descriptor, endpoints,
  simple descriptors) y, sin definición para ese `modelID`, arma una
  **definición generada** a partir de los clusters ("Not supported", pero
  con lo que expone): On/Off → `state`, Level → brillo, 0x0402 →
  temperatura, 0x0405 → humedad, Power Config → batería, IAS Zone →
  alarma, y para los clusters cliente (un botón) las acciones de On/Off y
  Level. Ata y configura reportes como lo haría para un equipo conocido, y
  lo publica a Home Assistant por el descubrimiento MQTT. Su página del
  equipo tiene "Reconfigure" e "Interview" para repetir todo, y el log de
  Zigbee2MQTT muestra cada mensaje.
- **ZHA:** no necesita *quirk*: crea entidades por cluster (un "cluster
  handler" cada uno): luz o interruptor según el device ID, sensores de
  temperatura, humedad y batería, un `binary_sensor` de apertura para una
  zona IAS 0x0015 (ZHA hace la inscripción sola), y los comandos de un
  cliente On/Off llegan como `zha_event` (en el log o en un disparador de
  automatización). Su "Zigbee device signature" muestra lo que leyó:
  sirve para comparar con lo que la placa dice que declaró.
- Los dos guardan el equipo por su IEEE: si con la misma IEEE cambian los
  endpoints, Home Assistant se queda con lo viejo hasta reconfigurar o
  volver a unirlo. Por eso cada tipo emulado lleva **su propia IEEE**
  (`ezb_nwk_set_extended_address()`, `nwk.h`, antes de arrancar): la del C6
  con el último byte según el tipo. **A medir** que la pila la tome con el
  RCP (`ezb_plat_radio_set_extaddr` existe en la parte "idf").

## 3. El diseño en P4OS

### Archivos

- `components/aos_154/aos_zigbee_emu.c` (nuevo): los perfiles, las señales
  del modo router, los tres manejadores (crudo, APS, acciones), el registro y
  los valores. En PSRAM, como el resto (`psram.lf`).
- `aos_zigbee_p4.c`: `stack_task` arma router o coordinador según el modo;
  en emulación, ni el gateway ni `aos_zigbee_devices_init()`.
- `aos_zigbee_nvs.c` (nuevo, chico): el `__wrap_nvs_open_from_partition` del
  camino A, y la línea en `components/aos_154/CMakeLists.txt`.
- La app (`aos_app_zigbee.c`), el portal (`aos_portal_154.c`), el simulador
  (`sim/aos_154_link_sim.c`) y los textos en los cinco idiomas.

### La API (en `aos_zigbee.h`)

```c
/* Emulating a device: the board joins someone else's network (Home
 * Assistant's coordinator) as a router showing one of these kinds, instead
 * of forming its own (aos_zigbee_emu.c, docs/plan/ZIGBEE-EMULAR.md). One
 * network at a time: the board's own stays in its NVS namespace, untouched. */
enum { AOS_ZBE_LIGHT_TEMP = 0,      /* stage 1: ep 1 on/off light, ep 2 temperature + humidity */
       AOS_ZBE_TH, AOS_ZBE_CONTACT, AOS_ZBE_RELAY, AOS_ZBE_DIMMER, AOS_ZBE_BUTTON, AOS_ZBE_KINDS };
enum { AOS_ZBE_OFF = 0, AOS_ZBE_SEARCHING, AOS_ZBE_JOINED, AOS_ZBE_PAUSED, AOS_ZBE_LEFT, AOS_ZBE_FAILED };
/* the values the board shows and the coordinator reads or drives */
enum { AOS_ZBE_V_TEMP = 0,          /* 0.01 degC */
       AOS_ZBE_V_HUM,               /* 0.01 % */
       AOS_ZBE_V_BATTERY,           /* 0.5 % */
       AOS_ZBE_V_ON, AOS_ZBE_V_LEVEL, AOS_ZBE_V_OPEN, AOS_ZBE_V_N };

typedef struct {
    uint8_t  ep;
    uint16_t cluster, attr;
    uint16_t min_s, max_s;          /* as the coordinator configured them */
    int32_t  change;                /* reportable change, raw units */
} aos_zigbee_emu_rep_t;

typedef struct {
    uint8_t  state, kind;           /* AOS_ZBE_* */
    uint8_t  ieee[8];               /* the emulated device's, little-endian */
    uint8_t  channel;
    uint16_t pan, nwk;              /* the network's PAN, the board's short address in it */
    uint8_t  xpan[8];
    int8_t   rssi;                  /* the last frame from the coordinator */
    bool     tclk;                  /* the trust center gave the board its own link key */
    bool     enrolled;              /* IAS: the coordinator enrolled the zone */
    uint16_t identify_s;            /* > 0: asked to identify itself, seconds left */
    int32_t  v[AOS_ZBE_V_N];
    uint8_t  nrep;
    aos_zigbee_emu_rep_t rep[8];
    uint32_t joined_ms;
} aos_zigbee_emu_t;

/* One thing the coordinator asked (or the board sent), for the log */
typedef struct {
    uint32_t ms;
    uint8_t  out;                   /* 0 asked of the board, 1 sent by it */
    uint16_t profile, cluster;      /* profile 0: a ZDO request */
    uint8_t  ep, cmd;
    char     text[80];              /* already put into words (English: the UI translates the kind) */
} aos_zigbee_emu_log_t;

/* Networks open for joining around (an active scan, ~4 s), for choosing one */
typedef struct { uint16_t pan; uint8_t xpan[8], channel; int8_t rssi; bool open; } aos_zigbee_net_t;
int  aos_zigbee_emu_scan(aos_zigbee_net_t *out, int max);

/* Join as kind, into the network with that extended PAN (NULL: any open).
 * With the board's own network running, false and *restart set: the
 * switch needs a restart (stage 1), which the caller asks the person for. */
bool aos_zigbee_emu_join(int kind, const uint8_t xpan[8], bool *restart);
bool aos_zigbee_emu_set(int value, int32_t v);  /* AOS_ZBE_V_*: the attribute set, reported as configured */
bool aos_zigbee_emu_report_now(void);           /* every value reported now, whatever was configured */
bool aos_zigbee_emu_press(int cmd);             /* the button: 0 off, 1 on, 2 toggle */
bool aos_zigbee_emu_pause(void);                /* stack stopped, the network kept for later */
bool aos_zigbee_emu_leave(void);                /* leaves the network (it is told) and forgets it */
bool aos_zigbee_emu_home(void);                 /* back to the board's own network */
void aos_zigbee_emu_info(aos_zigbee_emu_t *out);
int  aos_zigbee_emu_log(aos_zigbee_emu_log_t *out, int from, int max);
```

### Lo que se guarda

- En las preferencias (`"p4os"`): `zbe_mode` (0 red propia, 1 emular: con
  qué arranca la pila la próxima vez), `zbe_kind`, `zbe_vals` (los últimos
  valores, para que después de un reinicio reporte lo mismo) y `zbe_boot`
  (arrancar la pila sola en el próximo encendido: el reinicio del cambio de
  rol). Ninguna guarda un secreto.
- La red de Home Assistant (su clave, PAN, *bindings*, reportes) en el
  namespace `"zbemu"` de la pila, hasta "Salir". Fuera de los respaldos.
- El registro sólo en RAM (un anillo de 256 en PSRAM); "Guardar" lo escribe
  en `<tarjeta>/zigbee/emular-<fecha>.txt` para mandarlo o compararlo.

### La pantalla: la pestaña "Emular"

Una quinta pestaña en la app Zigbee (`TABS` en `aos_app_zigbee.c`: Canales,
Escuchar, Equipos, Mi red, **Emular**). Tres momentos:

1. **Elegir.** Las tarjetas de los tipos (en la etapa 1, una sola: "Luz y
   sensor de temperatura"), con lo que va a declarar (device ID, clusters,
   IEEE). Abajo: "En Home Assistant, abrí *Agregar dispositivo* (ZHA) o
   *Permitir unirse* (Zigbee2MQTT) y tocá **Unirse**". Si la red propia está
   andando: "Para emular, la placa deja su red un rato. Se reinicia y
   arranca como equipo. Tus equipos quedan guardados." con **Reiniciar y
   emular**.
2. **Buscando.** Las redes abiertas que se oyen (PAN, canal, señal; de
   `aos_zigbee_emu_scan`), la cuenta de los intentos (cada 5 s, hasta 3 min)
   y **Cancelar**.
3. **Adentro.** Arriba, "En la red 0x1A2B, canal 15, como 0x5C3E, señal -42
   dBm", con un punto si llegó la clave propia. Una tarjeta por función:
   - la luz: un foco grande que prende y apaga **Home Assistant** (un toque
     acá también la cambia, y Home Assistant tendría que enterarse);
   - el dimmer: el nivel que mandó, en una barra;
   - temperatura, humedad y batería: un deslizador cada uno (-20 a 50 °C de
     a 0,1; 0 a 100 %), con el reporte configurado al lado ("cada 10 s a 1 h,
     si cambia 1,00 °C") para que se entienda por qué un cambio chico no
     llega; **Reportar ahora**;
   - la puerta: un interruptor Abierta/Cerrada y "inscripta"/"sin inscribir";
   - el botón: **Prender**, **Apagar**, **Cambiar**.
   - "Identificate" de Home Assistant: la pantalla titila con un aviso los
     segundos pedidos.

   Debajo, **lo que pidió el coordinador**, lo último arriba, en palabras:
   "pidió el descriptor del nodo", "leyó Basic: fabricante, modelo, fuente",
   "ató On/Off (ep 1) a 0x0000", "configuró reportes de temperatura: 10 s a
   1 h, cambio 1,00 °C", "mandó Encender", "escribió la dirección del CIE",
   "pidió irse de la red". Al pie: **Pausar** (queda en esa red para
   después), **Salir de la red** (dos toques: Home Assistant la da de baja) y
   **Volver a Mi red**.

Al salir de la app no se para nada (como la red propia de hoy). Las
cadenas, con `_()` en castellano rioplatense; el texto del registro se arma
en inglés en el componente y la app traduce el tipo de pedido.

### El portal

En `aos_portal_154.c`, como los `zb*` de hoy:

| Pedido | Qué hace |
|---|---|
| `GET /api/154` | suma `zbemu`: `aos_zigbee_emu_info` |
| `GET /api/154/zbemu/log?from=N` | el registro desde N |
| `GET /api/154/zbemu/scan` | las redes abiertas |
| `POST /api/154/zbemu` `{"kind":"light_temp","xpan":"..."}` | unirse (`"restart":true` si hace falta reiniciar, y no lo hace sin `"confirm":true`) |
| `POST /api/154/zbemu/set` `{"temp":21.5,"hum":55,"battery":80,"on":true,"level":120,"open":false}` | los valores |
| `POST /api/154/zbemu/report`, `/press {"cmd":"toggle"}` | reportar ya, el botón |
| `POST /api/154/zbemu/pause`, `/leave`, `/home` | pausar, salir, volver |

En la página `#zigbee`, una sección "Emular" con lo mismo que la pestaña.
Con el portal se puede probar desde la Mac con `curl` mientras se mira Home
Assistant al lado.

### El simulador

`P4_SIM_ZB_EMU=1` en `sim/aos_154_link_sim.c`: un coordinador falso que a
los 3 s "abre" una red en el 15, une a la placa, hace la entrevista de
Zigbee2MQTT (los pedidos ZDO, el Basic, los *bindings*, los reportes) para
llenar el registro, y cada 20 s prende o apaga la luz. Para armar la
pantalla y sacar las capturas (con la interfaz en inglés) sin la placa.

## 4. Riesgos, lo que no se sabe, y por etapas

### Riesgos y dudas

| Duda | Por qué importa | Cómo se sale |
|---|---|---|
| Sin ZED en el P4 | un sensor de pila no se emula de verdad | aceptado; más adelante, un C6 o H2 aparte grabado desde el Programador con un ejemplo de Espressif de dispositivo final |
| Reiniciar la pila en el mismo encendido | sin eso, cambiar de rol o de tipo reinicia la placa | etapa 1 reinicia; etapa 2 prueba `esp_zigbee_stop` + `deinit` + `init` |
| El *wrap* de `nvs_open_from_partition` | si la librería guardara el handle entre arranques, mezclaría | con el reinicio no pasa; mirar el log de `nvs_report` (`aos_hal_p4.c`, los namespaces) |
| Lugar en `prefs` (256 KB) | dos redes en vez de una | medir con `nvs_report` después de unirse |
| El intercambio de clave | un coordinador viejo puede no darlo; uno nuevo puede exigirlo | `true` por defecto, y una opción "sin intercambio de clave" |
| `max_children = 0` | que no enrute o que el coordinador no lo acepte | medir; si no, 1 o 2 |
| La IEEE propia por tipo | que la pila use la del C6 igual | mirar en Home Assistant la IEEE que muestra |
| Zone Status Change sola | la puerta no avisaría | medir; si no, el comando a mano |
| La red del cliente queda en la placa | es su clave | "Salir" la borra; avisarlo en la pantalla mientras está en pausa |
| Equipos de la red propia mucho tiempo sin coordinador | algún Tuya podría irse | mirar al volver; nada se borra de la lista |
| La coexistencia con el Wi-Fi | un router escucha siempre, como el coordinador | la misma regla del C6 (BAJO al escuchar, ALTO al mandar, `ZIGBEE.md`); medir igual |

### Etapa 0: sin la placa (la puede hacer una sesión sola)

La API con el modo en las preferencias, `aos_zigbee_emu.c` con un perfil, la
pestaña, el portal y el coordinador falso del simulador, con los textos.
Compila el firmware pero no se instala.

### Etapa 1: lo más chico que sirve (con el usuario)

Un router con **ep 1 luz On/Off (0x0100)** y **ep 2 temperatura y humedad
(0x0302 + 0x0405)**, fabricante `P4OS`, modelo `P4OS-EMU-LT`, IEEE la del
C6. Cambio de rol por reinicio. Lo que se mira, en orden:

1. Antes de nada, **preguntarle al usuario qué coordinador tiene** (ZHA o
   Zigbee2MQTT, y el *dongle*: EmberZNet o Z-Stack) y que la red propia de
   la placa puede quedar apagada un rato.
2. El usuario abre la red en Home Assistant; la placa: `STEERING` 0, canal,
   PAN, dirección corta, `TCLK_READY`. **Lo hace el usuario.**
3. El registro de la entrevista: qué pidió y en qué orden, y si algo quedó
   sin respuesta (en Zigbee2MQTT, "Interview successful"; en ZHA, el equipo
   con sus entidades). Una foto de la pantalla de Home Assistant.
4. Home Assistant prende y apaga la luz: la placa la muestra, y el estado
   vuelve a Home Assistant (reporte). Tocar la luz en la placa: ¿Home
   Assistant se entera?
5. El deslizador de temperatura: cuánto tarda en verse en Home Assistant,
   contra el reporte que configuró; "Reportar ahora".
6. Reiniciar la placa: vuelve a la red de Home Assistant sola.
7. "Salir de la red" (Home Assistant lo ve irse) y "Quitar dispositivo"
   desde Home Assistant (la placa recibe el Leave).
8. **Volver a Mi red:** la red propia vuelve, el TS0002 contesta, los de
   pila aparecen cuando hablan. El Wi-Fi como siempre (la bajada de
   `c6.bin` de `ZIGBEE.md`).
9. Medir: lo ocupado en `prefs`, la RAM interna y la PSRAM libres.

### Etapa 2: los demás tipos

Puerta (IAS con inscripción), relé, dimmer y botón, cada uno con su IEEE; la
elección de la red por PAN extendido; y probar rearrancar la pila sin
reiniciar la placa. Con el usuario, cada tipo contra su coordinador; si
tiene los dos (ZHA y Zigbee2MQTT), contra los dos.

### Etapa 3 y más

- Código de instalación (`ezb_secur_ic_set`) para coordinadores que lo
  exigen.
- **La huella de un modelo real** (fabricante y modelo de, por ejemplo, un
  SNZB-02D) para ver si el convertidor de Zigbee2MQTT o el *quirk* de ZHA de
  ese modelo anda: el convertidor puede esperar clusters propios de la marca
  que la placa no tiene. Sólo a pedido del usuario y para probar su equipo.
- Más tipos con lo que ya sabe leer la placa (`aos_zigbee_fmt.c`: luz,
  presión, CO2, energía, cortina, termostato).
- Emular algo de Thread o Matter: otro documento.
