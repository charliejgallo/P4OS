# P4OS — La Tienda

Una app del sistema que lee un catálogo publicado en GitHub y baja a la
tarjeta las apps y los juegos, con todo lo que cada uno trae: su `.so`, sus
packs de arte, su página del portal, sus idiomas y sus módulos. Con fuentes
de terceros: cualquiera puede publicar un catálogo en su propio repo y quien
quiera lo agrega en su placa, sabiendo lo que hace.

**Estado (2026-11-20): fase 0 escrita, probada en la Mac y medida en la
placa en lo que no pide internet ni al usuario** (abajo, "Fase 0"): el
chequeo de símbolos, instalar y quitar en caliente y la bajada a la
tarjeta andan, también contra GitHub de verdad por TLS. Falta un corte del
Wi-Fi y un reinicio a mitad de bajada. Se trabaja en la rama `tienda` (worktree
`.claude/worktrees/tienda`, como se hizo con `ethernet`) y se mergea a
`main` cuando esté probada en la placa. Este documento es el plan; lo que
se vaya midiendo se anota acá, como en [ETHERNET.md](ETHERNET.md).

## Qué resuelve

Hoy una release trae `apps.zip`, `web.zip`, `lang.zip` y los packs sueltos
(la 0.28.0: unas 60 apps, 11 MB de `.so`, y 15 packs que suman ~110 MB; la
0.29.0 agrega los 13 de Bastión). Quien arma una
placa baja todo, lo descomprime y lo copia a la tarjeta en las carpetas
correctas; quien actualiza no sabe qué cambió. Con la Tienda:

- Una tarjeta vacía alcanza: se elige qué instalar desde la pantalla.
- Cada app se actualiza sola, sin esperar a copiar todo otra vez.
- Los packs grandes (Monster Hop, 37 MB) se bajan sólo si se quiere el juego,
  y los optativos (los mundos de Bastión) de a uno.
- El CI deja de arrastrar los packs de release en release (`CARRIED_PACKS`,
  `attach-packs.yml`): el catálogo apunta a la release donde está cada uno.
- Alguien de afuera puede publicar apps para P4OS sin pasar por este repo.

Lo que **no** es: no hay cuentas, pagos, reseñas ni servidor propio. Todo
sale de archivos estáticos en releases de GitHub.

## Lo que hay hoy, leído del repo y medido

| | |
|---|---|
| Cargar una app sin reiniciar | `aos_dynapp_load()` y `aos_dynapp_unload()` existen (`aos_dynapp.c`), pero sólo los usa el arranque: `install_apps.sh` reinicia la placa. Hay que probarlos en caliente |
| El cliente HTTP (`aos_http.c`) | HTTPS con el bundle común de la IDF, tres pedidos a la vez, HTTP/1.0. Guarda la respuesta **entera en PSRAM**: no escribe a un archivo, no sigue redirecciones, no reanuda. La Radio (`aos_radio.c`) sí sigue redirecciones: de ahí se saca |
| GitHub, visto el 2026-11-20 | `releases/latest/download/<archivo>` contesta 302 a `github.com/.../download/<tag>/...`, y ese otro 302 a `release-assets.githubusercontent.com`, que entrega con `Range` (206) aunque el pedido sea HTTP/1.0. Dos redirecciones, dos hosts, dos handshakes (1,6-1,8 s cada uno en la placa) |
| Los certificados | `github.com` cuelga de USERTrust ECC y los de descarga de ISRG Root X1: los dos están en el bundle común que ya lleva el firmware. No hay que cambiar el bundle |
| La API de GitHub | 60 pedidos por hora por IP sin token, y un JSON enorme por release. **No se usa** |
| Bajada por Wi-Fi | **~0,44 MB/s hacia la placa**, medido con `aos_fetch` (abajo): Monster Hop, un minuto y medio; todos los `.so`, 25 s. Los 1,2 MB/s de `/api/fs/get` son de la placa hacia afuera |
| Compatibilidad | `AOS_ABI_VERSION` 100 fijo, pero la tabla de símbolos crece con cada firmware: un `.so` nuevo puede pedir un símbolo que un firmware viejo no tiene, y hoy eso falla recién al cargarlo |
| Versiones | Las apps **no tienen versión**: el descriptor trae id, nombre, ícono, colores, banderas y orden |
| El ícono | Un blob AIC de hasta 512 B dentro del `.so`: entra en el catálogo, así la Tienda dibuja el ícono sin bajar la app |
| La red de seguridad | Modo seguro (BOOT apretado: sin apps de la tarjeta), vigilante de cuelgues con volcado, sha-256 en el HAL (`aos_hal_sha256_*`), `.part` + renombrar al escribir |
| Qué puede una app | Todo. Es código nativo dentro del firmware, sin protección de memoria (`docs/APPS-P4.md`). No hay permisos que mostrar ni que negar |
| Descomprimir | El firmware enlaza zlib (por libpng), así que un inflate hay; los paquetes van sin comprimir igual: son chicos, y así se leen sin tener el archivo entero en memoria |

## Decisiones

**D1. El catálogo es un archivo estático en la release, no la API.**
`https://github.com/<dueño>/<repo>/releases/latest/download/p4os-store.ndjson`.
"latest" es la última release **publicada**: un borrador o una pre-release
no llegan a nadie, así que publicar la release sigue siendo el momento en
que el usuario decide. Sin límite de pedidos, sin token, sin servidor.

**D2. NDJSON, una app por línea.** La primera línea describe la fuente; cada
una de las siguientes, una app. Se lee línea por línea a estructuras
compactas en PSRAM: nunca hay un árbol JSON del catálogo entero en memoria
(cJSON pide de a pedacitos, y eso cae en la RAM interna). Es el formato que
ya usa `/redes`. Estimado: ~1,5 KB por app, ~100 KB hoy.

**D3. Un paquete por app: `<app>.p4app`.** Un zip **sin comprimir** con las
rutas tal cual van en la tarjeta (`apps/bastion.so`, `apps/emulador/nes.so`,
`web/bastion.js`, `lang/en/demo.bastion.lang`) y un `p4app.json` adentro.
Una bajada, un hash, una instalación entera o nada. Se abre y se arma con
cualquier `zip`, y sirve igual sin la Tienda: soltarlo en el portal o
dejarlo en la tarjeta o en un pendrive lo instala por el mismo camino. Los
packs de arte van aparte, cada uno con su hash: son grandes, cambian poco y
algunos son optativos.

**D4. La versión es la release en que la app cambió por última vez**, y la
calcula el CI con git (el último commit que tocó `apps/<app>/`), no el hash
del binario: si dos compilaciones del mismo fuente no dan el mismo `.so`,
todas las apps aparecerían "actualizadas" en cada release (a medir en la
fase 1). El sha-256 es sólo para saber que lo que llegó es lo publicado.
"Hay una versión nueva" es que la del catálogo es distinta de la instalada,
como hace el Programador con sus firmwares. Una app que no cambió sigue
apuntando al archivo de la release vieja: no se vuelve a subir.

**D5. Dos chequeos de compatibilidad.** El catálogo dice `fw_min` por app
(el CI lo saca de un archivo nuevo, `symbols.since`, que anota en qué
versión apareció cada símbolo exportado): la Tienda muestra "Necesita P4OS
0.31" **antes** de bajar. Y la placa, después de bajar y antes de
registrar, recorre los símbolos sin definir del `.so` contra su propia
tabla: ese es el que manda, y cubre los paquetes sueltos y los de terceros,
cuyo `fw_min` puede estar mal. Una placa con firmware viejo puede además
pedir el catálogo de **su** versión (`releases/download/<su tag>/...`) para
seguir instalando lo que le sirve.

**D6. Instalar sin reiniciar, si la fase 0 lo confirma.** Cerrar la app si
está abierta, `aos_dynapp_unload`, reemplazar archivos, `aos_dynapp_load`.
Si en caliente no es confiable (módulos compartidos como `lua.so`, PSRAM
partida), queda "Reiniciar para terminar", que es lo de hoy.

**D7. Un recibo por paquete instalado**, en la tarjeta (`/apps/.store/`):
de qué fuente vino, qué versión, qué archivos puso y sus hashes. Con eso se
desinstala limpio y se detecta lo cambiado a mano. Los **datos** de la app
(partidas, notas, configuración) no son del paquete: al quitar una app se
pregunta si se borran también, y por omisión quedan. Una app copiada a mano
se reconoce por el hash de su `.so` contra el catálogo; si no coincide, se
muestra como "instalada a mano" y la Tienda no la toca sin preguntar.

**D8. Las fuentes se agregan sólo en la placa**, igual que las reglas del
portal y las claves de SSH: entrar al portal no alcanza para sumar una
manera de meter código. La lista vive en las preferencias, no en la
tarjeta: una tarjeta ajena no le agrega fuentes a la placa. El portal puede
*proponer* una fuente; se acepta en la pantalla, leyendo el aviso.

**D9. Sin firmas.** Va con lo abierto del OS (`docs/SECURITY.md`). Lo que
protege: HTTPS contra GitHub para el catálogo, y el sha-256 del catálogo
para cada archivo. Quien controla el repo de una fuente controla lo que
esa fuente instala; por eso agregar una fuente es la decisión de confianza,
y se toma en la placa.

**D10. Cuarentena.** Abrir un `.so` es ejecutar su código, también en el
escaneo del arranque. La placa anota (en memoria que sobrevive al reinicio,
como los contadores de los vigilantes) qué paquete recién instalado está
abriendo; si se cae ahí, en el arranque siguiente lo aparta
(`.so` → `.so.off`), arranca sin él y avisa. Una app rota no deja la placa
en un bucle, y el modo seguro sigue estando.

**D11. PSRAM.** El catálogo, la cola, los búferes de bajada y de TLS, en
PSRAM. La pila de la tarea de bajada también si no toca la flash (la del
cliente HTTP quedó interna porque lee una preferencia: acá esa lectura se
hace antes de lanzar la tarea). Lo que termine en RAM interna se justifica
con el número, en `docs/MEMORY.md`.

**D12. La placa no llama a GitHub por su cuenta sin permiso.** Al abrir la
Tienda, sí. Buscar actualizaciones en segundo plano (una vez por día, sólo
el catálogo) es un interruptor, apagado de fábrica. El pedido no lleva nada
que identifique la placa: `User-Agent: P4OS/<versión>`.

## El catálogo

Dos líneas de muestra (los tamaños y las versiones son de ejemplo):

```
{"schema":1,"source":{"name":"P4OS","by":"…","url":"https://github.com/…","about":{"es":"Las apps oficiales","en":"The official apps"}},"release":"v0.31.0","base":"https://github.com/<dueño>/<repo>/releases/download/"}
{"id":"demo.bastion","file":"bastion","name":{"es":"Bastión","en":"Bastion"},"about":{"es":"Defensa de torres en 3D…","en":"…"},"cat":"juegos","ver":"v0.29.0","abi":100,"fw_min":"0.29.0","pkg":{"url":"v0.29.0/bastion.p4app","size":183204,"sha256":"…","so_sha256":"…"},"packs":[{"file":"bastion_p4.pak","parts":3,"url":"v0.29.0/","size":21000000,"sha256":["…","…","…"],"need":true},{"file":"bastion_luna_p4.pak","name":{"es":"Mundo: Luna"},"url":"v0.29.0/","size":3100000,"sha256":["…"],"need":false}],"icon":"<AIC en base64>","colors":["1C2A4A","0B1020"],"shots":["v0.29.0/bastion-1.jpg"],"needs":["tarjeta"],"license":"Apache-2.0","src":"https://github.com/…/apps/bastion"}
```

- `file` es el nombre del `.so` sin extensión: es la clave del cargador y
  tiene que ser única en la placa. `id` es el del descriptor.
- Un paquete puede registrar varias apps (`lua.so`, `python.so`): la unidad
  de la Tienda es el paquete.
- `packs[].need`: sin él el juego no arranca (se baja con la app) o es un
  extra que se elige (un mundo, una mesa de Pinball).
- `needs`: lo que la app pide y la placa puede no tener (`usb-host`, `c6`,
  un módulo del header). Y `user_files`, para lo que la Tienda **no**
  distribuye y pone el usuario (el WAD de Doom, el GRP de Duke): la página
  de la app lo dice antes de instalar.
- Campos que la placa no conoce se ignoran; `schema` sube sólo cuando algo
  deja de entenderse.

De dónde sale cada cosa: un `apps/<app>/store.json` por app en git (nombre
y descripción en es/en, categoría, packs, necesidades, licencia, capturas,
si se publica), y `tools/gen_store.py` en el CI, que arma los `.p4app`, los
tamaños y hashes, la versión (D4), `fw_min` (D5) y el ícono (`tools/aic.py`
ya ensambla el AIC desde el `.c`). Los packs que no se arman en el CI se
anotan en `store/packs.json` (nombre, release donde está, tamaño, hash): es
lo único que el autor actualiza a mano al subir un pack nuevo.

## Fuentes de terceros

Una fuente es una dirección `https://` de un catálogo. En la placa se
escribe `usuario/repo` y la Tienda arma la dirección de GitHub; una
dirección entera también vale (GitLab, un servidor propio).

Al agregarla, la placa baja la primera línea, muestra quién dice ser y este
aviso, que hay que aceptar ahí mismo:

> Las apps de esta fuente no las revisó nadie de P4OS. Una app corre con
> todos los permisos del sistema: lee y escribe la tarjeta, ve las
> contraseñas guardadas, usa la red y los pines. Agregá sólo fuentes de
> gente en la que confiás.

Reglas:

- **Lo oficial no se pisa.** Un paquete no puede traer un `file` ni un `id`
  que ya es de otra fuente; entre terceros gana la que estaba primero, y se
  dice por qué se rechazó la otra.
- **Un paquete sólo escribe lo suyo:** bajo `apps/`, `web/` y `lang/`, con
  su nombre. Nada de `..`, rutas absolutas ni archivos de otro paquete.
- **Siempre se ve de dónde vino:** el nombre de la fuente junto a la app en
  la Tienda, en su página y en Ajustes, Almacenamiento.
- **Quitar una fuente** ofrece quitar sus apps; si quedan, dejan de
  actualizarse y se marcan así.
- **Fuente de desarrollo:** una dirección `http://` de la red de casa (la
  Mac que compila), sólo con el interruptor de Ajustes, Desarrollador y
  sólo en una red de confianza. Es también lo que usan las pruebas contra
  `tools/fake_github.py`.

Más adelante, un **índice de la comunidad**: un archivo en el repo oficial
con fuentes conocidas, que entran por pull request con dos condiciones
(código abierto con su licencia, y nada con copyright ajeno). Listada no
es revisada: el aviso es el mismo.

Para que alguien pueda publicar hace falta lo que hoy sólo existe dentro de
este repo: las cabeceras, la configuración de LVGL del firmware (que es
ABI), la tabla de símbolos y los chequeos de `build_apps.sh`. Eso sale como
`p4os-sdk-<versión>.zip` en cada release, con un repo plantilla (el de
`hello_app`) cuyo CI compila, chequea y publica catálogo y paquete al
poner un tag.

## En la placa y en el portal

- **La app Tienda** (del firmware, `aos_app_store.c`): Destacadas y
  categorías, Buscar, Actualizaciones, Instaladas, Fuentes. La página de
  una app: ícono, descripción, capturas, tamaño (con packs), versión,
  fuente, lo que necesita, y un botón que es Obtener / Actualizar / Abrir.
  Los extras (mundos, mesas) se tildan de a uno.
- **Las bajadas siguen con la Tienda cerrada:** el ícono aparece en el
  inicio atenuado con su avance, como en el iPhone; la Tienda lleva en su
  ícono el número de actualizaciones.
- **El servicio** (`aos_store.c`) es aparte de la pantalla: catálogos,
  compatibilidad, cola de bajadas (una a la vez), verificación, instalar,
  quitar, recibos. La bajada a archivo, con redirecciones, reanudación por
  `Range` y hash al vuelo, es una pieza del HAL (`aos_fetch`), igual en el
  simulador; después se le puede prestar a las apps (Mapas baja sus zonas).
- **El portal:** `GET/POST /api/store` y una página Tienda para mirar,
  instalar y quitar desde el navegador, y para soltar un `.p4app`. Las
  fuentes se ven pero no se agregan desde ahí.
- **El simulador** no carga `.so` (las apps van enlazadas): ahí la Tienda
  hace todo hasta dejar los archivos en `sim_fs`, y registrar se simula.

## Riesgos, y qué hay que medir antes de escribir la interfaz

1. **La bajada con la interfaz viva:** RAM interna mínima durante dos
   handshakes y la escritura a la tarjeta, fps mientras tanto, y que un
   corte del enlace con el C6 se reanude en vez de empezar de nuevo.
2. **Cargar y descargar en caliente:** veinte vueltas de instalar,
   actualizar y quitar, mirando la PSRAM partida y la caché del escaneo.
3. **Compilaciones repetibles:** si el mismo fuente da el mismo `.so` en
   el CI. Si no, D4 se queda como está escrita; si sí, se simplifica.
4. **GitHub cambia de hosts o de redirecciones** cuando quiere: nada en el
   código puede depender de cuántas son ni a dónde van.
5. **El reloj:** sin hora no hay TLS. La Tienda lo dice ("esperando la
   hora de la red") en vez de "falló la red".
6. **Un corte de luz en medio de instalar:** el recibo se escribe último;
   en el arranque, lo que quedó a medias (`.part`, archivos sin recibo de
   una instalación empezada) se limpia.
7. **Lo que no se distribuye:** nada con copyright. Doom y Duke siguen
   como hoy, el motor sí y los datos los pone cada uno.

## Fase 0: lo escrito, lo probado y lo que falta medir

**Escrito (2026-11-20), en la rama:**

- **`aos_fetch`** (`components/aos_hal/aos_fetch.c`, igual en la placa y en
  el simulador, sobre `aos_tls.c`): un GET a un archivo de la tarjeta.
  Sigue redirecciones entre hosts (nunca de https a http), escribe a
  `<archivo>.part` en bloques de 64 KB desde un búfer que la tarjeta toma
  directo, reanuda con `Range` lo que haya quedado, reintenta con espera
  (hasta cinco veces seguidas sin un byte nuevo) **desde la dirección
  original**, porque la firmada de GitHub vence, y al final lee el archivo
  de vuelta para su sha-256: lo que se verifica es lo que quedó en la
  tarjeta. Recién ahí le pone el nombre. Crea las carpetas que falten. Dos
  a la vez como mucho. Todo en PSRAM, la pila también (8 KB).
- **`aos_dynapp_check()`**: lee la tabla de símbolos dinámicos de un `.so`
  y le pregunta al mismo resolvedor que usa el cargador, sin cargar nada
  ni ejecutar una línea de la app. Dice cuántos símbolos faltan y cuáles,
  y rechaza lo que no es RISC-V de 32 bits o no es `ilp32f`. Es el
  segundo chequeo de D5.
- **Instalar y quitar en caliente:** `aos_dynapp_unload_file()` (todas las
  apps de un `.so`), `aos_dynapp_load()` ya no registra dos veces el mismo
  archivo, y los lugares de la tabla que deja una app quitada se vuelven a
  usar (antes se perdían: veinte vueltas la llenaban).
- **Para medir, a mano:** `/api/store/fetch`, `/check`, `/load`, `/unload`
  (`aos_portal_store.c`; el servicio de la fase 2 los reemplaza) y
  `tools/store_probe.py`, que los maneja y saca los números.
- **`tools/fake_github.py`**: las dos redirecciones, el segundo host, la
  dirección firmada de ~1 KB, los 3 KB de cabeceras, `Range`, y lo que
  sale mal a pedido (cortar a los N bytes, sin rangos, en chunks, 503,
  firmas de un solo uso, un bucle, bajar de https a http, lento).

**Probado en la Mac:**

- `tests/fetch_host/run.sh`: 20 casos contra el GitHub falso, todos pasan.
  La bajada entera con sus dos redirecciones; dos cortes con firmas de un
  solo uso (nueve conexiones: cada reintento vuelve a la primera
  dirección); cancelar y seguir (viaja sólo el resto, contado del lado del
  servidor); un servidor sin rangos; chunks; un `.part` más largo que el
  archivo y uno de otro archivo (lo rechaza el hash y el intento
  siguiente sale limpio); tamaño distinto; 404 sin reintentos; 503 que
  pasa; bucle de redirecciones; archivo vacío; carpetas nuevas; y por TLS
  con una autoridad de prueba: todo https, la bajada a http rechazada,
  un certificado de nadie (sin reintentos) y la placa sin hora (reintenta
  y se rinde diciendo por qué).
- Por el portal del simulador (`sim/build-tienda`, puerto propio): 5 MB con
  dos cortes y firmas de un solo uso, idéntico al original; cancelado a
  los 2 MB y terminado después con los 2,5 MB que ya estaban; un hash malo
  no deja archivo.
- El firmware `rev1_3` de la rama compila sin avisos: 6 963 184 bytes de
  los 8 323 072 de la ranura.

**Medido en la placa (2026-11-20)**, con el firmware de la rama
(`v0.29.0-2-g1d585ba`, por OTA, confirmado; contra lo que corría, `3b52992`,
sólo cambia lo de esta fase):

- **El chequeo de símbolos:** los 60 `.so` de la tarjeta (11,3 MB) dan
  bien. 10 ms cada uno de mediana, 20 ms el más lento (`rf.so`), 634 ms los
  60. Con copias adulteradas de `hello_app.so`: un símbolo renombrado lo
  nombra (`lv_label_creatX`); algo que no es ELF, un ELF de otro chip, el
  ABI sin float, el archivo cortado a la mitad y una tabla de secciones
  fuera del archivo se rechazan, cada uno con su motivo.
- **En caliente:** `hello_app.so` veinte vueltas, 195 ms quitar y 207 ms
  poner, la PSRAM libre dentro de ±240 bytes y la RAM interna igual. Doom
  veinte (303 ms poner), `python.so` diez con sus 8 apps (1,5 s quitar,
  1,8 s poner), CAS seis (806 ms), Monster Hop diez (228 ms). 95 apps
  registradas antes y después; tras las 66 vueltas la PSRAM libre quedó
  812 bytes abajo de donde empezó. `menu.txt` no se toca: quitar y poner
  no escribe el orden del inicio. **D6 queda confirmada** para el caso
  simple.
- **La bajada:** 37 202 325 bytes desde el GitHub falso en la Mac (http,
  dos redirecciones, tres conexiones): 85,8 s, de los que 3,2 s son leer
  el archivo de vuelta para el hash (11,6 MB/s) y 0,2 s conectar. **441
  KB/s** mientras fluye. RAM interna: 888 bytes menos en el punto más
  bajo. LVGL a 66 fps antes y durante (el inicio quieto); la carga de los
  núcleos pasa de 17 % y 6 % a 26 % y 14 %. **D11 confirmada:** no cuesta
  RAM interna.
- **Cancelar y seguir:** cancelada a los 16 MB y pedida otra vez, siguió
  desde el `.part`; el servidor mandó exactamente los 21 228 405 bytes que
  faltaban y el hash dio bien.
- **La velocidad es la del enlace, no la de este código:** el mismo
  archivo subido por el portal (`PUT /api/fs/put`) entra a 391 KB/s. La
  placa recibe a ~0,4 MB/s (ventana TCP de 23 040 bytes, el C6 por SDIO);
  subir eso es un trabajo aparte del firmware, y a la Tienda le sirve: a
  esta velocidad los 110 MB de packs son cuatro minutos.

- **Contra GitHub de verdad** (la release v0.28.0, con el sí del usuario):
  `lang.zip` (749 783 bytes) en 5,6 s, de los que 2,8 s son conectar: tres
  conexiones TLS, dos a `github.com` y una a
  `release-assets.githubusercontent.com`, las dos verificadas con el bundle
  que el firmware ya lleva. Otra vez enseguida, 4,8 s y 2,4 s conectando.
  `monsterhop_p4.pak` (37 202 325 bytes): **101,6 s, 379 KB/s** con TLS
  (441 sin él, en la red de casa), 2,5 s conectando y 3,1 s el hash, sin
  un reintento. RAM interna: 2 264 bytes menos en el punto más bajo. El
  tamaño y el sha-256 de los dos coinciden con los que publica GitHub.
- **De paso:** la API de GitHub da el sha-256 de cada archivo de una
  release (`gh release view --json assets`, campo `digest`). Para la fase
  1: `store/packs.json` puede armarse desde ahí, sin bajar los packs.

**Lo que falta medir:**

| Qué | Por qué no se hizo | Qué se anota |
|---|---|---|
| Un corte de la red | hay que apagar y prender el Wi-Fi en la placa, a mano | que siga sola desde donde iba |
| Un reinicio a mitad de bajada | reiniciar la placa pide el sí del usuario | que el `.part` sirva tras el arranque |
| Usar la placa mientras baja | lo ve el usuario | si algo se traba con una app pesada al frente |
| Abrir una app recién puesta en caliente | es abrir una app en su pantalla | que arranque, y que una que estaba abierta se cierre bien al quitarla |
| Reemplazar el archivo entre quitar y poner | falta un `.so` de otra versión | que corra el código nuevo |

## Roadmap

Cada fase termina con algo que se puede probar y con sus números anotados
acá. La sesión de firmware es una sola a la vez: mientras esta rama tenga
firmware en la placa, lo sabe `PRUEBAS-PLACA.md`.

| Fase | Qué | Dónde | Terminada cuando |
|---|---|---|---|
| **0. Medir** | `aos_fetch` (bajar a archivo, redirecciones, `Range`, sha-256) y `tools/fake_github.py`; el chequeo de símbolos sin cargar; instalar y quitar en caliente por una API de prueba | firmware, simulador | Los riesgos 1 y 2 tienen números de la placa; D6 y D11 confirmadas o cambiadas |
| **1. Catálogo y paquetes** | `store.json` de las 60 apps, `tools/gen_store.py`, el `.p4app`, `store/packs.json`, `symbols.since`; el CI publica catálogo y paquetes en una **pre-release** de la rama. `tools/store_cli.py`: un cliente en Python que instala desde el catálogo al simulador o a la placa, y que es la especificación ejecutable | CI y herramientas, sin firmware | El catálogo valida, y una tarjeta vacía queda completa con `store_cli.py`; riesgo 3 medido |
| **2. El servicio** | `aos_store.c`: catálogo, compatibilidad, cola, verificar, instalar, quitar, recibos, adoptar lo copiado a mano, cuarentena; `/api/store` | firmware | Desde `curl` se instala, actualiza y quita una app en la placa contra la pre-release, sin tocar la pantalla; un `.so` roto a propósito termina apartado |
| **3. La Tienda** | La app, el avance en los íconos del inicio, el número de actualizaciones, la página del portal | firmware, portal | El usuario arma una tarjeta vacía sólo desde la pantalla |
| **4. Terceros** | Fuentes (agregar en la placa, el aviso, las reglas), fuente de desarrollo, soltar un `.p4app` en el portal o traerlo en un pendrive | firmware | Una fuente de prueba en otro repo instala una app; lo que debe rechazarse se rechaza (pisar lo oficial, rutas ajenas, firmware viejo) |
| **→ merge a `main`** | Release "la Tienda". `apps.zip`, `web.zip` y `lang.zip` siguen saliendo un tiempo para quien instala a mano | | Fases 0 a 4 probadas en la placa |
| **5. Para publicar** | `p4os-sdk-<versión>.zip`, el repo plantilla con su CI, `docs/STORE.md` (en inglés), el índice de la comunidad | CI, documentos | Alguien sin este repo compila y publica una app |
| **6. El sistema** | Idiomas y emojis como componentes; el firmware del C6; el firmware del P4 por su perfil, con la vuelta atrás que ya tiene la OTA; el primer arranque con la tarjeta vacía ofrece lo esencial | firmware | Una placa recién grabada se arma entera sin una computadora |

Fuera del plan por ahora: reseñas y estadísticas (piden servidor), apps
pagas, y permisos por app (pedirían aislar el código, que es otro
proyecto).

## Preguntas abiertas

1. **El nombre.** "Tienda" se entiende, pero no se vende nada. Alternativas:
   "Apps", "Catálogo". Recomendado: Tienda.
2. **Cuándo mergear.** Recomendado: con terceros incluidos (fase 4), así
   el formato del catálogo y las reglas de fuentes salen probados juntos;
   la alternativa es mergear tras la fase 3 con sólo lo oficial.
3. **El firmware por la Tienda** (fase 6): es cómodo, y es también la
   placa instalando firmware bajado de internet sin firma. Recomendado:
   sí, pero siempre con un toque del usuario en la pantalla, nunca solo.
4. **Lo que ya está en las tarjetas.** Al primer arranque con Tienda,
   recomendado: adoptar en silencio lo que coincide por hash y no tocar lo
   demás.
