# P4OS — La Tienda

Una app del sistema que lee un catálogo publicado en GitHub y baja a la
tarjeta las apps y los juegos, con todo lo que cada uno trae: su `.so`, sus
packs de arte, su página del portal, sus idiomas y sus módulos. Con fuentes
de terceros: cualquiera puede publicar un catálogo en su propio repo y quien
quiera lo agrega en su placa, sabiendo lo que hace.

**Estado (2026-11-20): fase 0 escrita, probada en la Mac y medida en la
placa en lo que no pide internet ni al usuario** (abajo, "Fase 0"): el
chequeo de símbolos, instalar y quitar en caliente y la bajada a la
tarjeta andan, también contra GitHub de verdad por TLS y con un reinicio a
mitad de bajada; falta un corte del Wi-Fi. **La fase 1 está hecha**
(catálogo, paquetes y el cliente de referencia; su CI corriendo con el tag
`v0.30.0-tienda.1`). **La fase 2 está hecha y probada, en la Mac y en la
placa**, cuarentena incluida. **La fase 3 (la app Tienda y la página del
portal) está en la placa y el usuario la vio andar**, con las imágenes de
cada app; una sesión aparte saca las capturas que faltan. **La fase 4
(fuentes de terceros) está escrita y probada en la Mac**; falta la placa. Se trabaja en la rama `tienda` (worktree
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
| Usar la placa mientras baja | lo ve el usuario | si algo se traba con una app pesada al frente |
| Abrir una app recién puesta en caliente | es abrir una app en su pantalla | que arranque, y que una que estaba abierta se cierre bien al quitarla |
| Reemplazar el archivo entre quitar y poner | falta un `.so` de otra versión | que corra el código nuevo |

## Fase 1: catálogo y paquetes (hecha en la Mac, 2026-11-20)

**Lo escrito:**

- **`tools/gen_store.py`**: `check` (los manifiestos como están en el
  árbol, sin compilar nada), `build` (un `<app>.p4app` por app y
  `p4os-store.ndjson`) y `packs` (pone `store/packs.json` al día con los
  archivos de una release: tamaño y sha-256 los da GitHub, no se baja
  nada).
- **Un `apps/<app>/store.json` por app** (60; `pato` y `blittest` con
  `"publish": false`), `store/source.json` (quién es la fuente y las
  categorías, con sus nombres en es/en: la placa no las lleva escritas) y
  `store/packs.json` (los 15 packs de la v0.28.0).
- **`tools/store_cli.py`**, el cliente de referencia: `list`, `status`,
  `install`, `adopt` y `remove` sobre una carpeta que hace de tarjeta
  (`--card`) o sobre una placa o el simulador por el portal (`--board`).
  Su cabecera dice, paso por paso, qué hace una instalación: es lo que
  `aos_store.c` tiene que hacer en la fase 2.
- **El CI** (`build.yml`): el job `packs` corre `gen_store.py check` en
  segundos, y el paso `bundle` arma paquetes y catálogo con lo recién
  compilado, contra el catálogo de la última release publicada.

**Lo que quedó decidido al escribirlo:**

- **D4 cambió, con el riesgo 3 medido:** las compilaciones del CI son
  repetibles (de v0.27.0 a v0.28.0 los 57 `.so` salieron idénticos; de
  v0.26.0 a v0.27.0 cambiaron 47 sin tocar su código, por cuatro cabeceras
  del firmware). La identidad de una app es el sha-256 de su paquete, que
  se arma igual byte a byte desde los mismos archivos, y `ver` es la
  release donde ese paquete apareció. Un paquete que no cambió no se
  vuelve a subir: el catálogo sigue apuntando a la release donde está.
  **Las releases viejas no se borran:** el catálogo las nombra.
- **`fw_min` se calcula** de las tablas de símbolos de los tags
  (`git show <tag>:components/aos_dynapp/aos_symbols.c`), sin archivo
  nuevo que mantener. Todo lo anterior a la v0.29.0 cuenta como "siempre
  estuvo".
- **El ícono sale del `.so`**: el blob AIC se encuentra adentro y se lee
  con el parser de `tools/aic.py` (40 apps); las que usan un ícono del
  firmware llevan su número (16) y las que usan una letra o un símbolo de
  la fuente, eso (`lua`, `camaras`).
- **Un pack tiene que estar en una release publicada antes del tag** cuyo
  catálogo lo nombra. Si un pack obligatorio no está en ninguna, la app
  queda fuera del catálogo con un aviso en la corrida, en vez de
  instalarse rota. Hoy le pasa a Bastión: cuando la 0.29.0 esté publicada,
  `tools/gen_store.py packs --tag v0.29.0` y un commit.
- **El recibo** (`apps/.store/<file>.json`): fuente, versión, el sha del
  paquete, cada archivo y cada parte de cada pack con su hash, y
  `"adopted": true` si la app estaba copiada a mano.
- **Lo que un paquete puede escribir:** `apps/<file>.so`, módulos en
  `apps/<carpeta>/*.so`, `web/*.js` y `lang/<código>/*.lang`. Nada más,
  nada comprimido, nada que el recibo de otro paquete ya tenga.
- **Un extra que la persona ya tenía sobrevive a la actualización** de su
  app, y un pack que ya está en la tarjeta con el hash correcto no se baja
  de nuevo.

**Lo probado:**

- `tests/store_host/run.sh` (26 casos, con apps de verdad y el GitHub
  falso): tarjeta vacía, archivos idénticos a los compilados, recibos,
  estado, un extra por nombre, una release siguiente en la que cambia una
  sola app (un paquete escrito, cinco que quedan donde estaban), la
  actualización, adoptar lo copiado a mano sin tocar lo que no coincide,
  quitar, y doce rechazos: otro hash, otro tamaño, firmware más nuevo,
  otro ABI, una ruta que sube con `..`, un archivo fuera de lugar, el
  `.so` de otra app, un archivo que el `p4app.json` no lista, uno
  cambiado después, un paquete comprimido, uno que dice ser otra app y un
  archivo que es de otro paquete. En cada rechazo la tarjeta queda como
  estaba.
- **Una tarjeta vacía completa:** las 58 apps con todos sus packs, 352
  archivos y 151,7 MB; los 124 `.so`, módulos, packs y páginas comparados
  con los compilados, ninguno distinto. El catálogo pesa 52 KB, unos 930
  bytes por app.
- **Por el portal del simulador** (`--board`): tres apps con sus packs
  instaladas, vistas al día y quitadas.
- El paso del CI, corrido en seco en la Mac con sus mismos comandos: 57
  apps, y Bastión afuera con su aviso.

**Después, el mismo día:** la 0.29.0 se publicó y los 13 packs de Bastión
entraron a `store/packs.json` (58 apps en el catálogo). `store_cli.py
--board` contra la placa de verdad, con dos apps chicas: vio las 60 como
copiadas a mano, adoptó `dados` y `hello_app` (sus `.so` son byte a byte
los del catálogo) y reinstaló `hello_app` con el cambio en caliente,
"registrada, sin reiniciar". Los dos recibos de prueba se borraron.

**La Tienda sale como 0.30.0** (decisión del usuario). Las pruebas de la
rama llevan tags `v0.30.0-tienda.N`; `v0.30.0` queda para el merge.

**El CI corrió (2026-11-21, tag `v0.30.0-tienda.1`, 1 h 50):** los cinco
jobs pasaron y quedó un borrador de release con 97 archivos: los 58
`.p4app` (17,0 MB) y `p4os-store.ndjson` (52,8 KB) entre ellos. Bajado y
revisado: 58 apps, los 23 packs apuntando a las releases donde están (12 a
la v0.28.0, 11 a la v0.29.0), y los paquetes con el tamaño y el sha-256 que
dice el catálogo, sin comprimir. Ese tag es anterior a las imágenes: el
catálogo del borrador no las trae. Para las próximas, el paso instala
Pillow; si no puede, el catálogo sale sin imágenes y avisa.

**Un dato que cambia lo de adoptar:** de los 58 `.so`, sólo 5 son byte a
byte iguales a los compilados en la Mac. Las compilaciones son repetibles
dentro del CI (medido entre releases), no entre el CI y una máquina de
desarrollo. O sea: "adoptar lo que coincide" va a reconocer lo que alguien
instaló desde una release, y no lo que se copió desde una compilación
propia; eso queda como "copiada a mano" hasta reinstalarlo desde la
Tienda. En la placa del usuario, casi todo es de compilaciones propias.

**Lo que falta de esta fase:**

- **La pre-release está publicada (2026-11-21, con el sí del usuario):**
  `v0.30.0-tienda.1`, marcada como pre-release y no como la última. Su
  catálogo y sus paquetes se bajan sin cuenta desde
  `.../releases/download/v0.30.0-tienda.1/` (200 y 206), y
  `.../releases/latest/download/p4os-store.ndjson` sigue contestando 404:
  "latest" es la v0.29.0, así que ninguna placa la ve por su cuenta.
- **Leída desde la placa (2026-11-21):** con el modo desarrollo prendido
  por el usuario en la pantalla, la placa apuntada al catálogo de la
  pre-release: lo refrescó desde GitHub en 4,9 s y quedó con los mismos
  52 772 bytes que se bajan desde la Mac (mismo sha-256); 58 apps, todas
  caben. Instalar `hello_app` desde ahí: 4,0 s, sin reiniciar, el recibo
  con la fuente y la dirección de GitHub, la RAM interna 1,5 KB abajo en
  el peor momento. **Es el camino completo, de GitHub a la pantalla.**
- **Lo que adoptar reconocería en esta tarjeta:** 5 de los 58 `.so` son
  los del CI (`cjump`, `dmx`, `flappy`, `hello_app`, `radio`); los otros
  53 son compilaciones propias y quedarían como "copiada a mano". El
  `hello_app` instalado era justo uno de los iguales: **reemplazar un
  `.so` por uno distinto del CI y verlo correr** sigue sin probar.
- La placa quedó otra vez en el catálogo de prueba de la Mac, que tiene
  las imágenes (57 apps, 194).
- **Leer las descripciones.** Las de los `store.json` son un borrador
  escrito desde los README; las de las apps sin README, sin verificar.
  Qué pack es obligatorio en Bastión (el base y Luna) y en Pinball
  (Zombis) es una suposición: lo confirma la sesión de cada juego.

**Para las sesiones de apps, cuando esto esté en `main`:** una app nueva
trae su `store.json` (categoría y una o dos frases en es/en; los packs, lo
que necesita y lo que pone el usuario, si hay); `tools/gen_store.py check`
dice si está bien.

## Fase 4: fuentes de terceros (escrita y probada en la Mac, 2026-11-21)

**En el servicio** (`aos_store.c`):

- **Varias fuentes, un catálogo.** La primera es la oficial; hasta cinco
  más las agrega la persona. Cada una guarda su catálogo en la tarjeta
  (`apps/.store/cat-<hash>.ndjson`) y al cargar se arma un solo texto: la
  línea de cada fuente y, debajo, las apps que ofrece. La lista de fuentes
  vive en las preferencias de la placa (`store_srcs`), no en la tarjeta.
- **Lo oficial no se pisa.** Una app cuyo `file` o `id` ya lo lista una
  fuente anterior queda afuera (se cuenta en `skipped` y va al registro),
  y como la oficial es siempre la primera, nadie la reemplaza. Entre dos
  de terceros gana la que se agregó antes.
- **Mirar antes de agregar.** `aos_store_probe()` baja el catálogo a un
  costado y devuelve quién dice ser la fuente y cuántas apps trae;
  `aos_store_source_add()` sólo agrega **esa** dirección recién mirada.
  Sin haber mirado, se niega.
- **Quitar una fuente**, con sus apps o dejándolas (quedan instaladas y
  ya no se actualizan). La oficial no se quita.
- **`usuario/repo`** (o `github.com/usuario/repo`) se vuelve la dirección
  del catálogo de su última release; una dirección `https://` vale tal
  cual.
- **`http://` sólo en modo desarrollo y en una red de confianza**
  (`aos_access_here_trusted`), y se revisa en cada bajada, no sólo al
  agregar: un catálogo https no puede mandar a bajar paquetes por http.
  El modo desarrollo (`store_dev`) también es lo que deja cambiar el
  catálogo oficial por uno de prueba. Se prende **sólo en la placa**.
- **Paquetes sueltos** (`aos_store_sideload`): un `.p4app` de la tarjeta,
  de un pendrive o soltado en el portal se instala por los mismos pasos,
  con un recibo de fuente `local`. No puede ser una app que una fuente
  ofrece ("está en la Tienda: instalala desde ahí") ni pisar una que una
  fuente instaló ("quitala primero"). El archivo del usuario no se toca:
  se trabaja sobre una copia.
- Las llamadas `aos_store_*` quedaron fuera de la tabla de símbolos que
  se les presta a las apps: agregar una fuente lo hace una persona.

**En la app Tienda:** un filtro más, **Fuentes**, que abre su pantalla:
cada fuente con su dirección, cuántas apps ofrece y cuántas quedaron
afuera por estar en otra; "Quitar" (pregunta si con sus apps o sólo la
fuente); "Agregar una fuente" (un campo y el teclado); y después de
mirarla, **quién dice ser y el aviso**, que hay que aceptar ahí:

> Las apps de esta fuente no las revisó nadie de P4OS. Una app corre con
> todos los permisos del sistema: lee y escribe la tarjeta, ve las
> contraseñas guardadas, usa la red y los pines. Agregá sólo fuentes de
> gente en la que confiás.

Una app de terceros lleva el nombre de su fuente en la lista, y en su
página una nota: "De X, no de P4OS: nadie de P4OS la revisó". El
interruptor de modo desarrollo está en esa misma pantalla (el plan decía
Ajustes, Desarrollador; quedó acá, junto a lo que habilita).

**En el portal:** el estado lista las fuentes y dice si el modo desarrollo
está prendido; las apps de terceros llevan su etiqueta; y hay un botón
para instalar un `.p4app`. **No hay** forma de agregar o quitar fuentes
ni de prender el modo desarrollo desde ahí.

**Lo probado:**

- `tests/store_fw_host/run.sh`, ahora 44 casos: una fuente http sin modo
  desarrollo se rechaza; agregar sin mirar se rechaza; mirada y agregada,
  muestra quién dice ser; sus apps al lado de las oficiales y nunca en su
  lugar (un `dados` ajeno queda afuera); instalar una app suya deja un
  recibo que dice de quién es; las fuentes sobreviven a un reinicio; un
  paquete suelto se instala y deja el archivo donde estaba; uno que es de
  la Tienda, o que pisa lo que una fuente instaló, se rechaza; y quitar
  la fuente con sus apps o sin ellas.
- En el simulador, con dos servidores falsos: la pantalla Fuentes,
  escribir con el teclado, mirar una fuente, el aviso, agregarla, y su
  app listada al final con el nombre de la fuente.
- El firmware `rev1_3` compila: 7 037 456 bytes.

**En la placa (2026-11-21):** firmware `v0.30.0-tienda.1-12-g9e506d9` por
OTA, confirmado, con los idiomas subidos. La primera instalación de esta
fase dejó 8 KB menos de RAM interna libre: las tablas nuevas del servicio
y de la pantalla (fuentes, cola, estado, galería) eran estáticos comunes.
Pasadas a PSRAM (`AOS_BSS_PSRAM`), la RAM interna libre a los 30 s quedó en
209 707 bytes, 2 KB más que antes de la fase 3. Con el modo desarrollo
apagado la primera fuente volvió a ser la oficial, como debe; el catálogo
de prueba sigue cargado desde la tarjeta hasta que se prenda el modo y se
lo apunte otra vez.

**Segunda tanda (2026-11-21, en la Mac; va en `v0.30.0-tienda.2`):**

- **Los fallos se dicen en el idioma de la persona.** Cada fallo del
  servicio lleva ahora su *clase* (`aos_store_err_t`: red, no está en el
  servidor, sin hora, certificado, tarjeta, paquete, reglas, firmware,
  catálogo, fuente, cancelado, memoria...) al lado de la línea en inglés,
  que queda para el registro. La app arma con la clase una frase
  traducida ("No se pudo instalar Flappy. No está en el servidor.") y
  muestra la línea del registro abajo, chica; el portal hace lo mismo
  (`last_kind` en `/api/store`).
- **El portal propone, la placa decide (D8).** `POST /api/store/propose`
  guarda una dirección (una sola, hasta el próximo reinicio) y nada más:
  no baja ni agrega nada. La pantalla Fuentes la muestra en una tarjeta
  con "Mirarla" y "Descartar", y la solapa Fuentes lleva un punto
  mientras espera; mirarla sigue el mismo camino que escribirla a mano,
  aviso incluido. Sin modo desarrollo, una dirección `http://` ni se
  guarda.
- **Un `.p4app` se abre desde Archivos:** lo toma la Tienda (los mismos
  pasos que uno soltado en el portal) y se abre su pantalla.
- **El modo desarrollo recuerda el catálogo de prueba:** apagarlo vuelve
  al oficial, prenderlo de nuevo trae el que estaba.
- **Tope mientras baja.** Un catálogo se corta apenas el servidor anuncia
  o manda más de 1 MB (antes se revisaba al terminar, con la tarjeta ya
  escrita), y una foto, a los 512 KB. Los paquetes y los packs no lo
  necesitan: vienen con su tamaño y `aos_fetch` los ata a él; un
  catálogo que no lo diga no instala.
- `tests/store_fw_host`: 48 casos (las clases de fallo una por una, la
  propuesta, el catálogo sin fin). En el simulador: el cartel traducido
  de un 404, la tarjeta de la propuesta y "Mirarla" hasta el fallo
  traducido de una fuente que no existe. El firmware `rev1_3` compila:
  7 045 920 bytes.
- **En la placa (2026-11-21):** el firmware del tag `v0.30.0-tienda.2`
  por OTA, confirmado, con los idiomas subidos. RAM interna libre a los
  100 s: 209 811 bytes, lo mismo que antes de la tanda. Por la API: un
  fallo dice su clase (`not_listed`) y una propuesta que no es una
  dirección se rechaza con su texto traducido.
- **Los handoffs de apps** piden desde ahora la ficha y las capturas a
  cada app nueva (`docs/APPS-P4.md`, "In the store", y los internos).

**Lo que falta de esta fase:**

- **Que el usuario la recorra en la placa:** el teclado, el aviso, y
  `aos_access_here_trusted` con una fuente http (el simulador no
  distingue redes); y de la segunda tanda, proponer una fuente desde el
  portal y abrir un `.p4app` desde Archivos.

## Fase 3: la app y la página del portal (escritas, vistas en el simulador, 2026-11-21)

**La app Tienda** (`components/aos_apps/aos_app_store.c`, del firmware,
id `aos.store`): es la cara del servicio y nada más. Dos pantallas:

- **La lista:** el nombre de la fuente y su release, una fila de filtros
  que se desliza (Todo, Actualizaciones con su número cuando hay, cada
  categoría del catálogo, Instaladas) y una fila por app: su ícono (el
  blob AIC que trae el catálogo, uno del firmware, o una letra), el
  nombre, dos líneas de descripción y un botón que dice lo que toca:
  Obtener, Actualizar, Abrir, Reinstalar, o "P4OS 0.31.0" cuando el
  firmware no alcanza.
- **La página de una app:** el ícono grande, categoría, versión y tamaño
  (con los packs que necesita), el botón y "Quitar" (pide un segundo
  toque), la descripción, lo que pone el usuario, lo que necesita del
  hardware, y los **extras de a uno** (un mundo, una mesa), con un tilde
  los que ya están en la tarjeta.
- Arriba, mientras hay un trabajo, una tarjeta con lo que hace y su
  barra; si el último falló, por qué. La bajada sigue con la app cerrada.
- En Instaladas, si hay apps copiadas a mano, una tarjeta lo explica y
  ofrece "Que las maneje la Tienda" (adoptar). No se adopta nada sin que
  la persona lo pida.
- Abrir la Tienda con red pide el catálogo (D12); sin red o sin catálogo,
  lo dice y ofrece reintentar.
- Con la app cerrada, `aos_app_store_service_tick()` mantiene el número
  de actualizaciones sobre su ícono del inicio.
- Lee cada línea del catálogo una vez a un registro chico en PSRAM y lo
  suelta al cerrarse.

**La página Tienda del portal** (`pageTienda` en `app.js`, entrada nueva
del menú): la fuente y el catálogo, el trabajo en curso con su barra,
buscar de nuevo, adoptar, cancelar, y la lista con filtro y búsqueda:
instalar (eligiendo extras), actualizar, quitar y abrir en la placa.
Dibuja los íconos con el mismo intérprete de AIC que la página Inicio.

**Idiomas:** 44 textos nuevos del sistema y 28 del portal, traducidos al
inglés y al alemán (`lang/en`, `lang/de`) y embebidos en el firmware.

**Visto en el simulador** (capturas por su portal, contra el GitHub
falso): la lista con sus 58 apps y sus íconos, la página de Bastión con
sus diez mundos, instalarla tocando el botón (la barra avanzando, después
"Abrir" y "Quitar"), un extra tocado y bajado, los filtros, la pantalla
acostada, y la página del portal en un navegador con una app instalada.
El firmware `rev1_3` compila: 7 010 944 bytes.

**En la placa (2026-11-21):** firmware `v0.30.0-tienda.1-5-g38a94dd` por
OTA (y después `v0.30.0-tienda.1-7-g8d94f7f`, con las imágenes y los datos
de cada app), confirmado; 96 apps, la Tienda entre ellas; los idiomas subidos
(`install_lang.sh en de`) y el inglés recargado. Como el catálogo oficial
todavía no existe (la release publicada no lo trae), la placa quedó
apuntando a uno de prueba servido desde la Mac con `fake_github.py`: las
58 apps, todas `by_hand`. **Es provisorio:** al terminar las pruebas se
vuelve a la dirección oficial (`POST /api/store/refresh {"url":""}`) y se
apaga ese servidor.

**Imágenes y datos de cada app (2026-11-21, pedido del usuario tras verla
en la placa):**

- **Las capturas** salen de `docs/img`, que ya las tenía: cada
  `store.json` nombra hasta cuatro (`"shots"`) y `gen_store.py` le arma a
  la Tienda una copia a su medida, un JPEG de 560 px de lado mayor (el
  decodificador de la placa no toma progresivos), con el hash en el nombre
  para que una imagen que no cambió conserve su dirección. Hoy son 91
  imágenes, 2,4 MB en total, ~26 KB cada una; el catálogo pasó a 60 KB.
  Necesita Pillow; sin él el catálogo sale sin imágenes y lo avisa.
- **Los datos extra** (`"feat"` y `"langs"`) los calcula el generador
  leyendo el código de cada app: packs de arte, joystick, página en el
  portal, si vive sólo de pie o acostada, red, sonido, Bluetooth, y los
  idiomas que trae su paquete. Lo que el código no dice va en `"tags"` del
  `store.json` (hoy, `3d`).
- **En la página de la app:** una fila de etiquetas con su símbolo (3D,
  Arte en packs, Joystick, De pie, Sonido, ES DE EN, USB, Header...) y,
  debajo, las imágenes una al lado de la otra, que se deslizan. El
  servicio baja cada una a `apps/.store/shots` una sola vez
  (`aos_store_shot`), un hilo aparte la decodifica y la página la pone en
  su marco cuando llega; se sueltan al salir de la página.
- La página del portal muestra lo mismo: etiquetas y miniaturas.
- Visto en el simulador, de pie y acostada.

**Cobertura de imágenes, hoy:** de las 58 apps publicadas, 12 tienen tres
o cuatro, 7 tienen dos, 31 una sola y 8 ninguna (Cámaras, Clima,
Cotizaciones, Hola, Lua, Mila, Notas, Grabadora). **El usuario quiere tres
o cuatro en todas:** son unas 110 capturas nuevas del simulador, en
inglés como las demás, cada app en una pantalla que valga la pena. Es un
trabajo de apps (no toca el firmware): las capturas van a `docs/img` y sus
nombres a `"shots"` del `store.json`; `tools/gen_store.py check` dice si
quedó bien.

**Lo que falta de esta fase:**

- **Que el usuario la use en la placa:** el dedo, el desplazamiento y
  la velocidad de armar la lista (58 íconos) sólo se ven ahí.
- **El avance sobre el ícono del inicio** mientras una app se instala,
  como en el iPhone: pide que el inicio sepa mostrar una app que todavía
  no está registrada. No está hecho; hoy el avance se ve en la Tienda.
- Los mensajes de error del servicio están en inglés (van al registro):
  la app los muestra tal cual. Falta traducir los que ve la persona.
- Capturas (no hay; el catálogo las prevé en `shots`), y que el buscador
  del sistema encuentre apps de la Tienda.

## Fase 2: el servicio (hecha, 2026-11-21)

**Lo escrito:**

- **`components/aos_store`** (`aos_store.c`, igual en la placa y en el
  simulador): el catálogo, instalar, actualizar, quitar, adoptar y los
  recibos, en un hilo propio que hace un trabajo a la vez desde una cola
  de ocho. No tiene pantalla. Hace los mismos pasos que `store_cli.py`,
  con una diferencia a favor: **todo se verifica y se deja listo en
  `apps/.store/tmp` antes de tocar nada instalado** (el paquete, cada
  archivo con su hash, cada `.so` contra la tabla de símbolos con
  `aos_dynapp_check`, y los packs), y recién entonces saca la app del
  inicio, mueve los archivos a su lugar, escribe el recibo y la vuelve a
  registrar. Un archivo que ya está en la tarjeta con los mismos bytes no
  se reescribe.
- **El catálogo en memoria es su texto** (52 KB en PSRAM) y una tabla
  chica por app; una línea se pasa por cJSON cuando hace falta. Se guarda
  en `apps/.store/catalog.ndjson`, así la Tienda abre con el último que
  bajó. Uno que no se lee, o de un esquema que este firmware no conoce,
  no reemplaza al que sí.
- **Un lector de zip sin compresión** propio (el directorio al final, cada
  entrada con sus desplazamientos revisados contra el tamaño del archivo).
- **La API del portal** (`aos_portal_store.c`): `GET /api/store` (el
  trabajo en curso y su avance, cómo terminó el último, y cada app con su
  estado: `absent`, `installed`, `update`, `by_hand`), `GET
  /api/store/catalog` (el NDJSON tal cual), y `POST` a `refresh`,
  `install`, `remove`, `adopt` y `cancel`. La dirección del catálogo es
  una preferencia (`store_url`), para probar; las fuentes son de la fase 4.

**Lo probado:**

- `tests/store_fw_host/run.sh`: 31 casos, el servicio en C compilado en la
  Mac contra el GitHub falso **y contra `store_cli.py`**. El mismo
  catálogo instalado por cada uno en su tarjeta deja los mismos archivos
  byte a byte y los mismos recibos, y cada uno lee los recibos del otro.
  Además: no bajar de nuevo un pack que ya está, un extra por nombre que
  sobrevive a una actualización, adoptar (con su pack si está entero, sin
  él si no, y sin tocar lo que no coincide), quitar, un firmware viejo
  para el catálogo, quince paquetes que hay que rechazar dejando la
  tarjeta como estaba (los de `store_cli.py` más un zip que no es zip,
  uno cortado, una carpeta de más y un archivo entre los recibos), y dos
  catálogos que no deben pisar al bueno.
- **Por el portal del simulador:** refrescar, instalar tres apps (Bastión
  por su id, con un mundo extra), leer el catálogo y quitarlas.
- El firmware `rev1_3` compila sin avisos: 6 986 080 bytes (+23 KB).

**La cuarentena (D10), escrita el 2026-11-21** (`aos_dynapp.c`,
"Quarantine"): el cargador anota el nombre del `.so` que está abriendo en
PSRAM que el arranque no borra, escrita al chip en el momento (un pánico
no da otra oportunidad). Un arranque que sigue a una caída y encuentra un
nombre ahí le cuenta una falta; **dos seguidas contra el mismo archivo** y
lo renombra a `<archivo>.so.off` antes del escaneo, arranca sin él y lo
avisa en pantalla. Dos y no una, porque la placa puede caerse por otra
cosa mientras carga una app sana; una carga buena borra la cuenta. Cubre
el escaneo y el abrir una app; no una app que se cae mientras corre (eso
no es un bucle). El servicio la muestra como `set_aside` y reinstalarla es
la vuelta. Además el servicio limpia `apps/.store/tmp` al iniciar.

**Medido en la placa (2026-11-21)**, firmware `v0.30.0-tienda.1-2-ga3b4e8d`
por OTA, contra el GitHub falso en la Mac:

- El escaneo completo tras el firmware nuevo, con el registro de la
  cuarentena activo: las 95 apps registradas, ninguna apartada.
- **Refrescar:** 1,4 s; 58 apps, las 58 `by_hand` (nada tiene recibo
  todavía) y todas caben en este firmware.
- **Memoria:** con el catálogo cargado, 192 bytes menos de RAM interna y
  140 KB menos de PSRAM. Durante los trabajos la interna bajó como mucho
  1,6 KB.
- **Instalar sobre lo que está, en caliente:** 1,8 s, `hello_app` sigue en
  el inicio, 95 apps.
- **Un paquete cuyo `.so` pide un símbolo que este firmware no tiene:**
  rechazado en 0,5 s ("needs a newer P4OS: this one lacks
  lv_label_creatX"), el `.so` instalado sin tocar, la app en el inicio y
  `tmp` vacío. Es el segundo chequeo de D5, ya dentro del servicio.
- **Quitar y volver a poner sin reiniciar:** 95 → 94 → 95 apps.
- Al terminar: la dirección oficial otra vez (que hoy contesta 404: la
  release publicada no tiene catálogo todavía), y el recibo y el catálogo
  de prueba borrados de la tarjeta.

**La cuarentena en la placa (2026-11-21, con el sí del usuario):** una
copia de `hello_app.so` con la primera instrucción de `aos_app_abi`
ilegal, subida como `zz_crash.so` (el chequeo de símbolos la da por
buena: no ejecuta nada) y cargada en caliente. La placa se cayó ahí; en
el arranque siguiente, "strike 1 of 2", el escaneo la abrió y se cayó
otra vez; en el tercero, "strike 2 of 2" y "set aside as
zz_crash.so.off", antes del escaneo. Volvió a contestar a los 17 s de la
primera caída, con sus 95 apps y el firmware confirmado. El criterio de
la fase 2 está cumplido. No se vio el aviso en pantalla (dura ocho
segundos, a los doce del arranque): falta mirarlo. El archivo de prueba
se borró y el volcado de la caída quedó marcado como leído.

**Lo que falta de esta fase:**

- Una app con pack grande bajado por el servicio en la placa (la bajada
  sola ya se midió en la fase 0).
- Adoptar todo lo que hay en la tarjeta: cambia 58 recibos y reescribe
  páginas e idiomas que difieran; mejor contra el catálogo oficial, cuando
  exista.

**De la fase 0, el reinicio a mitad de bajada (hecho, 2026-11-20):** la
primera vez, reiniciada a los 15 MB, la placa volvió con un `.part` de 0
bytes y bajó todo de nuevo: la FAT anota el tamaño al cerrar o
sincronizar. `aos_fetch` ahora sincroniza cada megabyte. Con ese firmware
(`v0.29.0-5-gefef5a5`) y la misma prueba: reiniciada a los 15 138 816
bytes, volvió con 14 680 064 en la tarjeta (se perdieron 458 752, menos
del megabyte), siguió desde ahí, el servidor mandó exactamente los
22 522 261 que faltaban y el hash dio bien. Sincronizar no cuesta
velocidad: dos bajadas enteras de 37 MB a 472 y 479 KB/s (441 antes).

## Roadmap

Cada fase termina con algo que se puede probar y con sus números anotados
acá. La sesión de firmware es una sola a la vez: mientras esta rama tenga
firmware en la placa, lo sabe `PRUEBAS-PLACA.md`.

| Fase | Qué | Dónde | Terminada cuando |
|---|---|---|---|
| **0. Medir** | `aos_fetch` (bajar a archivo, redirecciones, `Range`, sha-256) y `tools/fake_github.py`; el chequeo de símbolos sin cargar; instalar y quitar en caliente por una API de prueba | firmware, simulador | Los riesgos 1 y 2 tienen números de la placa; D6 y D11 confirmadas o cambiadas |
| **1. Catálogo y paquetes** | `store.json` de las 60 apps, `tools/gen_store.py`, el `.p4app`, `store/packs.json`; el CI publica catálogo y paquetes en una **pre-release** de la rama. `tools/store_cli.py`: un cliente en Python que instala desde el catálogo al simulador o a la placa, y que es la especificación ejecutable | CI y herramientas, sin firmware | El catálogo valida, y una tarjeta vacía queda completa con `store_cli.py`; riesgo 3 medido |
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
