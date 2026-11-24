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

**Una fuente de prueba (2026-11-21):** `tools/store_sample.py` arma una
segunda fuente inventada, "Tienda de prueba", para recorrer esto en la
placa: genera cuatro apps chicas desde la plantilla, cada una con su id
(`prueba.*`), las compila y deja su catálogo y sus paquetes como una
release, para servir con `tools/fake_github.py --repo prueba/apps`. El
catálogo lista tres: dos propias (Contador de prueba, con una imagen, y
Colores de prueba) y una que se llama `dados.so` como la oficial, que la
placa tiene que dejar afuera. La cuarta sale como paquete suelto
(`suelta.p4app`, App suelta) para soltar en el portal o abrir desde
Archivos. Probada con el servicio en la Mac contra los dos servidores: la
fuente se mira y se agrega, queda con 2 apps y 1 salteada, instalar
`dados` trae el oficial, y el paquete suelto se instala. **En la placa
falta que el usuario la agregue.**

**El usuario la recorrió en la placa (2026-11-21)** con la fuente de
prueba: proponerla desde el portal, mirarla, el aviso, agregarla, la app
de terceros con su imagen, y el paquete suelto desde Archivos. Lo que
encontró, y lo que cambió por eso:

- **El paquete suelto se instalaba y no aparecía en la Tienda.** La lista
  salía sólo de los catálogos. Ahora el servicio agrega, después de las
  apps de las fuentes, **lo que está en la tarjeta y ninguna fuente
  ofrece**: primero lo que tiene recibo (un `.p4app` entregado, o una app
  de una fuente que se quitó dejando sus apps) y después cada `.so` de
  `apps/` del que no sabe nada. Cada una lleva una línea propia en el
  texto del catálogo (`"local": "pkg"` o `"hand"`, y `"from"` con la
  fuente que ya no está), `src` -1 y un `origin`
  (`AOS_STORE_LISTED/LOOSE/HAND`). Las sueltas se quitan como cualquier
  otra; ninguna de las dos se puede "instalar" (no hay de dónde), ni
  adoptar. Después de instalar, quitar, adoptar o tomar un paquete el
  servicio vuelve a leer esa lista.
- **Filtro por origen** (pedido): una segunda fila de chips, más chica,
  que aparece cuando hay algo que distinguir (otra fuente, un paquete
  suelto, algo copiado a mano): Cualquier origen, cada fuente por su
  nombre, Paquetes sueltos, Copiadas a mano. Se combina con el filtro de
  arriba. En el portal es un segundo desplegable. Al terminar de tomar un
  paquete, la Tienda queda mostrando los paquetes sueltos.
- En la página de una app sin fuente: el ícono es el del inicio si la app
  está cargada, su texto dice de dónde salió, y el pie dice el archivo.
  El pie de una app de terceros decía "P4OS": ahora dice su fuente.
- **En el portal no se veía la imagen de una app de terceros:** la página
  armaba todas las direcciones con la base de la primera fuente. Ahora
  cada app usa la de la suya, y las categorías de todas las fuentes se
  juntan.
- `tests/store_fw_host`: 52 casos (lo suelto y lo copiado a mano listados
  como lo que son, sus líneas en el catálogo, que no se instalan desde una
  fuente, quitar). Visto en el simulador con las dos fuentes de prueba: la
  fila de orígenes, cada filtro, la página de la app suelta; y en el
  navegador, la miniatura de terceros cargada y el desplegable. Firmware
  `rev1_3`: 7 051 520 bytes.
- **En la placa (2026-11-21):** por OTA, confirmado. Su lista pasó de 60 a
  63: la app suelta como paquete instalado, y `pato` y `blittest` (las
  dos que no se publican) como copiadas a mano. RAM interna libre a los
  70 s: 209 619 bytes, sin cambio.

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

## Parches, el firmware desde GitHub y más texto (pensado el 2026-11-21)

Tres pedidos del usuario antes del tag `v0.30.0-tienda.3`.

### 1. Actualizar con parches, como en DOMCOM-Dimmer

Allá un parche es un `esp_delta_ota` (detools + heatshrink): la placa lee
la app que corre de su ranura, le aplica el parche y escribe el resultado
en la otra, que se verifica como una imagen entera; el encabezado lleva el
SHA-256 de la app de la que se hizo, y un parche de otro firmware se
rechaza antes de escribir nada. Allá valía un mundo: por Zigbee, a 50
bytes por bloque, una imagen de 680 KB tardaba una hora y un parche de
11 KB, un minuto.

**Medido acá** con los `.bin` publicados del perfil `rev1_3` (los que
arma el CI), y detools 0.53:

| De → a | Imagen entera | Parche heatshrink | Parche lzma |
|---|---|---|---|
| v0.28.0 → v0.29.0 (sólo cambió la versión) | 6,95 MB | 109 KB (1,6 %) | 1,2 KB |
| v0.29.0 → v0.30.0-tienda.1 (el servicio de la Tienda) | 6,96 MB | 419 KB (6,0 %) | 202 KB (2,9 %) |
| la imagen comprimida, sin parche | | gzip 3,90 MB (56 %) | xz 3,29 MB (47 %) |

La placa baja de GitHub a ~0,38 MB/s: la imagen entera son unos 18 s, y
después hay que escribir los 7 MB en la otra ranura de todos modos (la
OTA por el portal, subida y escritura, tarda 35 s). Un parche ahorra esos
~17 s de bajada y 6,5 MB de datos, de un minuto en total. A cambio: la
librería en el firmware, un archivo de bases y un parche por cada versión
anterior y por cada perfil en el CI, y una vuelta a la imagen entera cada
vez que lo que corre no es exactamente una release (toda placa con un
firmware compilado a mano). En el único camino donde un fallo importa.

**Las apps:** los 58 paquetes pesan 17 MB entre todos; la mediana son
104 KB y 53 están debajo de 500 KB: una app baja en uno o dos segundos.
Un parche no ahorra nada que se note, y rompería la regla de que un
paquete es su SHA-256. Los bytes están en los packs de arte (151 MB,
Monster Hop solo son 37), que cambian poco y ya no se bajan de nuevo
cuando no cambiaron (por parte y por hash).

**Decisión propuesta: no ahora.** Ni para apps ni para firmware. El
formato queda abierto: la entrada del firmware en el catálogo puede
llevar más adelante `"patches": [{"from_sha256", "url", "size",
"sha256"}]` sin cambiar de esquema, y una placa que no los entienda baja
la imagen entera. Si un día importa (una red medida, un firmware mucho
más grande), el orden sería: primero la imagen comprimida (la mitad de la
bajada, sirve desde cualquier versión, sin bases), y recién después
parches.

### 2. El firmware desde GitHub, en Ajustes

Estaba en la fase 6; se puede adelantar porque casi todo existe:
`aos_fetch` (bajar a la tarjeta con reanudación, tamaño y SHA-256),
`aos_hal_ota_begin/write/end` (la otra ranura), el arranque a prueba con
vuelta atrás sola a los 30 s, y la página Ajustes → Actualización, que hoy
sólo muestra las dos ranuras y deja volver a la anterior.

- **El catálogo dice qué firmware hay**, en la línea de la fuente oficial:
  `"fw": {"ver": "v0.31.0", "images": {"rev1_3": {"url":
  "v0.31.0/p4os-rev1_3.bin", "size": ..., "sha256": "..."}, "rev3_x":
  {...}}}`. Lo escribe `gen_store.py` con los `.bin` que el CI ya publica.
  Como la placa lee el catálogo de la última release **publicada**, un
  borrador o una pre-release no le ofrece firmware a nadie.
- **Sólo de la fuente oficial** (la primera). Una fuente de terceros que
  traiga `"fw"` se ignora.
- **El firmware tiene que saber su perfil** (`rev1_3` o `rev3_x`): hoy no
  lo lleva escrito. Una imagen de otro perfil no se ofrece.
- **Pasos:** bajar a `apps/.store/tmp` (se reanuda si se corta), verificar
  tamaño y SHA-256 **antes de tocar la flash**, escribir la otra ranura,
  y pedir el reinicio. Arranca a prueba, como cualquier OTA.
- **Siempre con un toque en la placa** (o en el portal, que ya puede subir
  un firmware): nunca sola. Ajustes → Actualización muestra "Hay una
  versión nueva: vX", el botón, el avance y "Reiniciar ahora". La Tienda
  lleva ahí cuando una app dice "Necesita P4OS más nuevo".
- Sin firma (decisión del usuario): la confianza es la del catálogo, HTTPS
  a GitHub más el SHA-256 que el catálogo trae.
- Queda para después: el firmware del C6 por el mismo camino.

**Hecho (2026-11-21), con la imagen entera.** Es lo que vuelve útil el
`fw_min` de las apps.

- **El catálogo:** `tools/gen_store.py fw --catalog <ndjson>
  p4os-rev1_3.bin p4os-rev3_x.bin` escribe `"fw"` en la primera línea. Es
  un paso aparte porque las imágenes de los dos perfiles las arman
  trabajos distintos del CI: corre en el de la release, donde se juntan.
- **El perfil** sale de la configuración con la que se compiló
  (`CONFIG_ESP32P4_REV_MIN_300` es `rev3_x`), sin tocar el HAL.
- **El servicio** (`aos_store_fw()`, `aos_store_fw_install()`): un trabajo
  más de la cola, `firmware`. Baja a `apps/.store/tmp/firmware.bin`
  (`aos_fetch` lo ata al tamaño y lo vuelve a leer para su hash), y recién
  entonces lo pasa a la otra ranura de a 64 KB. Se puede cancelar en las
  dos mitades. No reinicia.
- **Qué es "más nueva":** la versión que corre es lo que dijo `git
  describe`. `v0.30.0` es más nueva que `v0.30.0-tienda.2` (un tag de
  prueba anterior a la release) y no que `v0.30.0-4-g1a2b3c4` (esa release
  y commits encima).
- **Ajustes → Actualización**, grupo "Desde GitHub": versión nueva, estado
  (bajando… %, escribiendo… %, lista), "Bajar e instalar vX", "Cancelar",
  "Buscar actualizaciones" y "Reiniciar ahora" (con segundo toque). La
  página Firmware del portal tiene lo mismo, y la Tienda lleva ahí desde
  una app que pide un P4OS más nuevo.
- **Probado en la Mac:** `tests/store_fw_host`, 61 casos (9 del firmware:
  la oferta, la imagen de cada perfil, un perfil sin imagen, qué es más
  nuevo, la que ya corre, una imagen que no es la publicada sin tocar la
  ranura, la ranura que la rechaza, y una fuente de terceros que dice
  `"fw"` y no ofrece nada). En el simulador, con una ranura de mentira
  (`P4_SIM_OTA=<archivo>`, `P4_SIM_FW=<versión>`): bajar 7 MB del
  servidor de prueba, el avance, cancelar, y el archivo escrito es byte a
  byte la imagen publicada. Firmware `rev1_3`: 7 062 096 bytes.
- **En la placa (2026-11-21):** el firmware `v0.30.0-tienda.2-16-g22d5eb7`
  por OTA común (el que corría no sabía actualizarse solo). Con el
  catálogo de prueba de la Mac ofreciendo `v0.30.0` (esa misma imagen,
  7 062 112 bytes) la placa la vio como más nueva, y pedida por la API la
  bajó y verificó en 27 s, la escribió en la otra ranura en 31 s (59 s en
  total) y quedó "lista": la otra ranura pasó a tener esa versión, en
  estado `new`. La RAM interna libre bajó 1,5 KB mientras duró (208 151
  de 209 611). El catálogo con las descripciones largas pesa 185 KB y la
  placa lo refresca en 2,6 s por la red local.
- **El usuario la recorrió en la pantalla (2026-11-21):** "Reiniciar
  ahora" arrancó la ranura escrita, a prueba, y se confirmó a los 32 s.
  Después el camino entero desde Ajustes → Actualización: bajar, ver la
  escritura y reiniciar con los dos toques; otra vez arrancó y se
  confirmó. Las dos ranuras quedaron con la misma versión, las dos
  `valid`. Del registro de ese arranque: la imagen de 7 062 112 bytes bajó
  en 16,0 s (0,44 MB/s, 0,6 s de ellos releyéndola para su hash), abrir
  la otra ranura (el borrado) llevó 11,6 s y escribirla 29,1 s: 57 s del
  toque a "lista". Durante el borrado el estado dice "escribiendo… 0 %".
- **Falta también:** que el CI lo arme de verdad (el paso nuevo del
  trabajo `release` no corrió nunca), y el firmware del C6.

### 3. Más texto en la página de cada app

`"desc"` en `store.json`: unos párrafos en `es` y `en`, texto plano, una
línea en blanco entre párrafos, 1200 caracteres como mucho por idioma. Va
al catálogo y **no al paquete**, así cambiar una redacción no hace un
paquete nuevo de una app que no cambió. La página de la app lo muestra
debajo de la oración corta; el portal, en "Leer más", y su buscador
también lo lee. Los textos de las 58 apps los escribe una sesión aparte,
desde los README y el código. **Hecho el campo (2026-11-21); los textos,
en curso.**

De paso, visto en el simulador: las imágenes de pie (315×560) mostraban
tres columnas de puntos sueltos en el borde derecho, porque el
decodificador no termina un bloque que queda cortado. El generador las
saca ahora con los dos lados múltiplos de 16 (320×560, 560×320).

### Las partes del sistema (2026-11-21)

Lo que faltaba para que una placa nueva no necesite copiar nada a mano:
las traducciones del portal (que sólo se leen de la tarjeta), los emojis
en color y el firmware del C6. Era de la fase 6.

- **No son paquetes.** Una parte del sistema es una línea del catálogo
  con `"sys": true` y una lista de archivos: cada uno, un archivo de una
  release con su tamaño y su SHA-256 y el lugar de la tarjeta adonde va.
  Se bajan a `apps/.store/tmp`, se verifican y se mueven. El recibo es
  esa lista.
- **Sólo la fuente oficial**, y sólo a `lang/<código>/_*.lang`,
  `fonts/*.pak` y `firmware/*.bin` (`sys_allowed()`). Una línea así en el
  catálogo de terceros se saltea y se cuenta.
- **`store/system.json`** las declara: `sys-lang-en`, `sys-lang-de`
  (sus dos archivos salen del árbol y se publican con la release como
  `lang-en-_sistema.lang`...), `sys-emoji` y `sys-c6` (archivos que una
  release ya tiene, por `store/packs.json`; `gen_store.py packs` ahora
  anota también `c6.bin`). Categoría nueva, "Sistema".
- **Lo que ya está en la tarjeta no se baja:** copiado a mano aparece
  como tal, y Obtener (o "Que las maneje la Tienda") lo hashea una vez y
  le escribe el recibo si es el del catálogo.
- **No se quitan desde la Tienda:** nada dice quién más los usa.
- **`emoji.pak` está abierto mientras la placa corre** (`aos_emoji.c` lo
  abre al arrancar) y FAT no reemplaza un archivo abierto: uno más nuevo
  queda al lado como `emoji.pak.new` y `aos_emoji_init()` lo pone en su
  lugar en el arranque siguiente, antes de abrirlo.
- **El C6:** la Tienda sólo deja `firmware/c6.bin`; instalarlo sigue
  siendo Ajustes → Actualización, y su página lleva ahí.
- En la app: sin "Abrir" ni "Quitar"; una tilde cuando está. En el portal
  lo mismo.
- `tests/store_fw_host`: 69 casos (8 de esto: listadas, instaladas en su
  lugar, no se quitan, tomadas por sus bytes sin bajar, el emoji nuevo
  esperando al lado, una parte que escribiría en otro lado, una con otros
  bytes, y una de terceros que no se ofrece). En el simulador: la solapa
  Sistema con las cuatro, instalar el firmware del C6, la página de una.
  Firmware `rev1_3`: 7 068 336 bytes.
- **En la placa (2026-11-21):** `v0.30.0-tienda.3-2-g7f31098` por OTA,
  confirmado; RAM interna libre 209 623 bytes, sin cambio. La tarjeta ya
  tenía los cuatro (idiomas, `emoji.pak`, `c6.bin`, copiados hace
  tiempo): los vio a todos como copiados a mano. Los dos idiomas,
  pedidos por la API, se instalaron desde el catálogo de prueba en 2,6 y
  3,1 s (dos archivos cada uno, 0,46 MB) y el portal quedó sirviendo las
  cadenas nuevas.
- **El usuario, en la pantalla:** Obtener en los emojis no bajó nada (el
  `emoji.pak` de la tarjeta era el del catálogo: lo hasheó y le escribió
  el recibo) y en el firmware del C6 bajó 1,4 MB en 3,0 s y reemplazó el
  de la tarjeta, que era de otra compilación. Las cuatro quedaron con su
  tilde.

### Los documentos públicos (2026-11-21)

Para el merge: `docs/INSTALL.md` (una placa nueva desde cero, venir desde
0.2x, sumar apps, el firmware desde GitHub), `docs/STORE.md` (qué hace y
qué revisa la Tienda, las fuentes de terceros, el formato de un catálogo,
las llamadas del portal, lo medido), el README con "Getting started" al
principio y la sección "The Store", la entrada 0.30.0 del `CHANGELOG.md`,
y el borrador de las notas de la release en
`docs/internal/RELEASE-0.30.0.md` (no está en git). Las capturas nuevas
son del simulador, en inglés.

## Contra GitHub de verdad, antes del merge (2026-11-21)

Los CI de los tags `v0.30.0-tienda.3` y `.4` terminaron bien, y con ellos
corrió por primera vez el paso del trabajo `release` que escribe el
firmware en el catálogo: los dos perfiles, 7 068 400 y 7 072 128 bytes. El
catálogo del `.4` trae 58 apps, las cuatro partes del sistema (los idiomas
como archivos de esa release; los emojis y el firmware del C6 apuntando a
las releases que ya los tenían) y una descripción larga en cada línea.

**Un defecto: salió sin imágenes**, como el `.1` y el `.2`. El `python3`
del contenedor de la IDF no tiene `pip`, y el aviso llegaba después de la
hora y media de las apps. Corregido: Pillow viene de `apt` (`python3-pil`)
en el primer paso, donde además se comprueba, y el catálogo se arma con
`/usr/bin/python3`. Visto en el mismo contenedor con un flujo de un solo
uso: Pillow 10.2.0, tres imágenes de `docs/img` a 320×560 y 560×320.
**Las imágenes en un catálogo del CI se ven recién en el próximo tag.**

Publicado el `.4` como pre-release (no es "latest": `releases/latest`
sigue en la 0.29.0, sin catálogo) y la placa apuntada a su catálogo, en
modo desarrollo. Todo por HTTPS a GitHub:

| | |
|---|---|
| El catálogo (180 KB, 62 líneas) | 4,1 s |
| Reinstalar Simon, que estaba copiada a mano: el `.so` del CI en lugar del de la Mac, registrada | 4,6 s |
| Las partes del sistema | las cuatro "instaladas": los idiomas del CI son byte a byte los que ya tenía |
| El firmware, 7,07 MB: bajar y verificar | 21,7 s (0,33 MB/s; 1,3 s de conexión) |
| El firmware: del pedido a "lista" | 64 s |
| RAM interna libre, el mínimo mientras tanto | 206 143 bytes |

La otra ranura quedó con `v0.30.0-tienda.4`, la imagen que armó el CI, en
estado `new`. El usuario la reinició desde Ajustes: **arrancó, a prueba, y
se confirmó a los 32 s**. Es la primera vez que la placa corre un firmware
compilado por el CI y traído por ella misma. RAM interna libre: 211 955
bytes; las 97 apps registradas; los emojis cargados.

Visto ahí mismo, y corregido: corriendo `v0.30.0-tienda.4`, la placa se
ofrecía ese mismo tag como "versión nueva". La regla ("una release es más
nueva que sus tags de prueba") trataba al tag que corre como uno de
prueba anterior. Ahora el tag que corre, o una compilación hecha unos
commits después, no es más nuevo que sí mismo. Con una release de verdad
(`v0.30.0`) no pasaba. Un caso más en `tests/store_fw_host` (70).

**Mergeado a `main` el 2026-11-21** (`2b554b8`, directo: `main` no se
había movido).

## La 0.30.0, publicada (2026-11-21)

El CI del tag `v0.30.0` terminó bien y su borrador se revisó antes de
publicar: 300 archivos, 58 paquetes, **199 imágenes** (las primeras que
arma el CI), los dos firmwares, las cuatro partes del sistema; todo lo que
el catálogo nombra está adjunto, y el firmware, un paquete y un idioma
bajados del borrador tienen el hash que el catálogo dice. Publicada como
la última release, con las notas (`docs/internal/RELEASE-0.30.0.md`).

La placa, vuelta al catálogo oficial (`releases/latest`), lo leyó en
4,8 s. El usuario quitó la fuente de prueba, apagó el modo desarrollo e
**instaló la `v0.30.0` desde Ajustes → Actualización**: arrancó a prueba y
se confirmó a los 32 s. Ya no se ofrece a sí misma (corre la que el
catálogo trae). RAM interna libre: 212 047 bytes. Los dos servidores de
prueba de la Mac están apagados.

Las releases de prueba `v0.30.0-tienda.1` a `.4` (dos pre-releases y dos
borradores) y sus tags se borraron el 2026-11-22, después de la 0.32.0:
el catálogo publicado no nombraba ningún archivo de ellas.

## Después del merge: que una app nueva no cueste dos horas de CI

Medido en el CI del tag `v0.30.0-tienda.2` (2026-11-21): de 108 minutos,
**101 son compilar las 60 apps una tras otra**. El firmware `rev1_3` son
4,8 minutos, el `rev3_x` 3,4, el del C6 2,1 y los packs medio. Cada app es
un proyecto ESP-IDF propio que recompila LVGL y los componentes de la IDF
enteros para sacar un `.so` de pocos KB: 1,7 minutos por app.

Con la Tienda, sumar una app ya no debería obligar a recompilar todo. En
orden, y **los dos primeros son la prioridad apenas esté el merge** (pedido
del usuario, que quiere poner varios agentes a hacer apps):

1. **Compilar sólo lo que cambió.** Una huella por app (sus fuentes, los
   headers compartidos, `aos_symbols.c`, la configuración de LVGL, la
   versión de la IDF); si es la de la release anterior, se reutiliza su
   `.p4app` en vez de compilar. El catálogo ya trabaja así con los
   paquetes (`--previous`): falta que el CI no compile para descubrirlo.
   Un cambio en un header compartido recompila todo, como debe. Una
   release con una app nueva: unos 10 minutos.
2. **Repartir las apps entre varias máquinas.** Una matriz de tandas en el
   trabajo de apps (GitHub da 20 trabajos a la vez en un repo público):
   seis tandas, unos 17 minutos de reloj cuando hay que compilar todo.
   Hay que juntar después los `.so` de las tandas para el paso que arma
   el catálogo.
3. **El kit** (el `p4os-sdk` de la fase 5): los headers, la configuración
   y las opciones del compilador armados una vez, y cada app compilada con
   eso en segundos, sin proyecto IDF. Es lo mismo que necesita un tercero
   para publicar sin este repo.
4. **Un catálogo que no viaje con el firmware.** Hoy cada catálogo es de
   una release de firmware. El formato ya deja que cada paquete viva en
   otra release, y cada app ya dice qué firmware mínimo pide: un flujo
   aparte podría compilar sólo la app que entró, subir su paquete y
   regenerar el catálogo, sin publicar firmware. El cuidado: esas apps se
   compilan contra los headers del firmware **publicado**, no los de
   `main`; el chequeo de símbolos ve una función nueva y no un cambio de
   estructura, para eso está el número de ABI.

Mientras tanto, sin cambiar nada: no sacar un tag por app (el costo es por
corrida, no por app), y que cada sesión valide en la Mac con
`tools/build_apps.sh <app>` y el simulador.

**Los puntos 1 y 2, hechos y en `main` el 2026-11-21** (`tools/ci_apps.sh`
y el flujo `build.yml`; PR 1, mergeado directo). Explicado para quien
llega, en `docs/BUILDING.md`, "The CI":

- Las apps se reparten en **seis tandas** que corren a la vez. Cada tanda
  guarda lo que compiló (`.appcache`, en la caché de GitHub) bajo una
  huella de lo que hace a una app: sus archivos, los headers y la tabla de
  símbolos contra los que se compila, la configuración, la IDF y el
  compilador. Con la misma huella el `.so` se copia y no se compila. Sólo
  se guarda un `.so` que pasó los chequeos de `build_apps.sh`.
- Un trabajo aparte, `store`, arma `apps.zip`, `web.zip`, `lang.zip`, el
  catálogo, los paquetes y las imágenes con los `.so` de las tandas, en
  una máquina común (ahí `pip install pillow` anda). Los trabajos de
  firmware quedan con el firmware y nada más.
- Una tanda que no tiene nada que compilar tampoco configura el firmware:
  la huella sale de los archivos del árbol.
- **Medido en la primera corrida del PR** (todo por compilar): 19 minutos
  de reloj contra 108; la tanda más larga, 18,7; `store`, 20 segundos. El
  catálogo salió con las 58 apps, las cuatro partes del sistema y las 199
  imágenes, y **los 58 `.so` son byte a byte los de la release 0.30.0**,
  que se compiló en serie.
- La caché de un tag no le sirve a nadie y no se guarda: un tag lee la de
  `main`. La de un PR es de ese PR.
- **La segunda corrida recompiló todo** (20 minutos): la caché se
  restauró bien, pero cada app estaba guardada bajo una huella tomada
  *después* de compilar, cuando la compilación ya había tocado archivos
  del árbol. Corregido: la huella se toma una vez, antes; no cuenta los
  `dependencies.lock` (el gestor de componentes los reescribe) ni al
  propio guion; y el registro dice qué tenía la caché de una app cuando
  la recompila. Probado en la Mac con una app: compilada, después copiada
  byte a byte sin compilar, y recompilada al cambiarle una línea.
- **El propio CI lo comprueba desde entonces:** un trabajo `reuse`, después
  de las tandas, hace una copia limpia del repo, trae la caché que esa
  misma corrida acaba de guardar y pregunta si algo se compilaría de nuevo
  (`tools/ci_apps.sh --check`). Si la huella de una app no sale igual dos
  veces, falla esa corrida y no una posterior en silencio. CAS queda fuera
  de la pregunta: su motor no está en git y es parte de su huella. No
  corre en un tag, que no guarda caché propia.
- **La corrida que lo probó** (la cuarta del PR): 21,3 minutos de reloj
  con todo por compilar, la tanda más larga 19,6, y los seis chequeos
  encontraron las 59 apps (todas menos CAS) sin compilar ninguna.
- **Falta medir en `main`** lo que cuesta una subida que sólo agrega o
  cambia una app. La primera corrida en `main` arma su propia caché (la
  del PR no le sirve) y tarda los 21 minutos; de ahí en más debería
  quedar en lo que tarda el firmware, 5 o 6.
- **Sin hacer, y a mano si molesta:** una subida que sólo toca documentos
  corre el CI entero. Se puede saltear con `paths-ignore`, cuidando que
  `docs/img` no entre en la lista: de ahí salen las imágenes del catálogo.

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

**Dónde quedó (2026-11-21):**

- **Fases 0 a 4, el merge y la release:** hechos. La 0.30.0 está
  publicada y la placa del usuario la corre, instalada por ella misma.
- **De la fase 6**, adelantado: el firmware del P4 desde GitHub y las
  partes del sistema (idiomas, emojis, el firmware del C6 como archivo).
  Falta: que el primer arranque con la tarjeta vacía ofrezca lo esencial,
  e instalar el firmware del C6 sin pasar por Ajustes.
- **De la fase 5**, hecho `docs/STORE.md`. Falta todo lo demás: el kit
  para compilar una app sin este repo (que es también el punto 3 del CI),
  el repo plantilla y el índice de la comunidad.
- **Del CI:** los puntos 1 y 2. Faltan el kit (3) y el catálogo que no
  viaja con el firmware (4).
- **Pedido del usuario (2026-11-22): una categoría "Nuevas"** en la
  Tienda, para ver de un vistazo lo que entró con la última release.
  **Hecho el mismo día**, en el firmware de la 0.32.0. `ver` no servía:
  es la release del paquete, y cambia con cada actualización de la app.
  El catálogo lleva ahora `first` en cada app (la release en que se la
  listó por primera vez, que `gen_store.py` arrastra de `--previous`) y
  `news` en la cabecera (la más nueva de esas): son nuevas las apps de
  esa release, hasta que otra traiga apps. El chip está después de
  "Actualizaciones", en la placa y en el portal. Con el catálogo de la
  0.31.0 como anterior, las nuevas de la 0.32.0 van a ser Hockey,
  Traductor, Oficina y Estudio. Probado en el simulador con un catálogo
  de prueba y en `tests/store_host` y `tests/store_fw_host`; en la placa
  se ve recién con el catálogo de la 0.32.0.
- **Los packs de la 0.32.0, publicados el 2026-11-22** en la release
  `v0.31.0` (la última publicada), desde la Mac: 41 archivos, 256 MB,
  cada uno comparado después con el local por tamaño y SHA-256. Son los
  cinco de idioma del Traductor (37 partes, opcionales), los tres de
  Hockey y el de fuentes de Oficina. `gen_store.py packs --tag v0.31.0`
  los sumó a `store/packs.json` (y volvió a mover `c6.bin`, que se dejó
  como estaba). Los de Hockey y Oficina entraron a `CARRIED_PACKS`; **los
  de idioma no se copian de release en release** (serían 245 MB más cada
  vez): quedan en `v0.31.0`, que por eso no hay que borrar.
- **La 0.32.0 salió el 2026-11-22** (tag `v0.32.0` en `b9192b5`), con
  Oficina, Traductor, Estudio y Hockey. Orden: packs a la última
  publicada, `store/packs.json`, `main` y su CI, tag, CI del tag (borrador
  con 83 archivos, 241 MB), notas, revisar, publicar. La revisión del
  borrador: los 398 archivos que nombra el catálogo, cada uno buscado en
  la release que dice (27 en `v0.32.0`, 70 en `v0.31.0`, 272 en `v0.30.0`,
  14 en `v0.29.0`, 15 en `v0.28.0`) con su tamaño, y 159 también por
  SHA-256. Publicada, la placa del usuario (en `v0.31.0-49-ge594fae`) ve:
  el firmware 0.32.0 para bajar, las cuatro apps en "Nuevas", y tres
  actualizaciones (Vuelos y los dos idiomas del sistema); Estudio le
  pide el firmware nuevo.
- **La 0.32.1 salió el 2026-11-22** (tag `v0.32.1` en `e7908c5`), tres
  horas después: un solo arreglo, la Tienda esperando una descarga libre.
  Sin packs nuevos. El borrador trajo 58 archivos y su catálogo nombra los
  mismos 398 que el de la 0.32.0, con sólo las dos imágenes de firmware
  propias de esta release; ninguna app cambió, y "Nuevas" sigue marcando
  las cuatro de la 0.32.0. Publicada, la placa (en `v0.32.0`) la ve como
  firmware para bajar y nada más.
- **Sueltos:** el avance sobre el ícono del inicio mientras una app se
  instala (fase 3); cortar el Wi-Fi en medio de una bajada, quitar una
  fuente y quitar un paquete suelto en la placa (`PRUEBAS-PLACA.md`); los
  símbolos de las partes del sistema en la página del portal; borrar las
  releases de prueba `v0.30.0-tienda.1` a `.4` de GitHub.
- **Lo que sigue por fuera de la Tienda:** `docs/plan/RESPALDO.md`. Sus
  cuatro etapas (el respaldo en la tarjeta, las copias que la placa hace
  sola, el pendrive, y un servidor propio por SFTP) están en `main` desde
  el 2026-11-21, para la 0.31.0, junto con SFTP hacia la placa
  (`docs/SSH.md`).
- **La 0.31.0 salió el 2026-11-22** (tag `v0.31.0` en `fcd36ab`), sin
  Estudio: seis apps nuevas, los respaldos y SFTP. Es la primera release
  que una placa baja sola, **y la placa del usuario la bajó ese mismo
  día** desde Ajustes: 27 s de bajada, escrita, arrancada a prueba y
  confirmada; y **las seis apps nuevas se instalaron desde la Tienda**,
  byte por byte las publicadas (`PRUEBAS-PLACA.md`). Cómo se armó:
  - Los 15 packs de arte nuevos (26 MB) se subieron a la release
    publicada `v0.30.0`, que es donde el catálogo los nombra, y recién
    después se anotaron en `store/packs.json` con
    `tools/gen_store.py packs --tag v0.30.0`, que lee de GitHub el tamaño
    y el SHA-256 de cada uno: dieron los de los archivos de la Mac.
  - **Ojo con ese comando:** además de los `.pak` mira `c6.bin`, que el CI
    compila de nuevo en cada release y no da los mismos bytes. Lo quiso
    pasar de `v0.29.0` a `v0.30.0`, y eso le habría ofrecido a cada placa
    una "actualización" de la radio que no es tal. Se dejó como estaba;
    mirar el diff de `packs.json` antes de subirlo.
  - Los packs nuevos entraron a `CARRIED_PACKS` (los dos workflows), para
    que cada release los lleve a la siguiente.
  - El CI de `main` con la caché: 8 minutos (las apps, un minuto por
    tanda). Su catálogo se bajó y se miró antes del tag: 64 apps.
  - Del borrador se comprobó, antes de publicar, que los 335 archivos que
    nombra el catálogo existen con su tamaño y su SHA-256 (34 en esta
    release, el resto en las tres anteriores), y que las dos imágenes
    dicen `v0.31.0`. Publicado, `releases/latest` sirve ese catálogo.
- **La 0.33.0 salió el 2026-11-24** (tag `v0.33.0` en `eef973cb`): nueve
  apps nuevas (Planeo, Tanques con dos DLC opcionales, Tortuga, DJ,
  Deriva, Vitral, Fusión, Monitor serie, Mi casa), portugués, francés e
  italiano como partes del sistema, y la Terminal para multímetros. Orden:
  los cuatro packs nuevos (Tanques y Tortuga) subidos a `v0.32.1`, la
  última publicada, y anotados con `gen_store.py packs --tag v0.32.1`
  (otra vez quiso mover `c6.bin`: se dejó como estaba) y en
  `CARRIED_PACKS`; `main` y su CI (26 min, recompiló todo por los
  encabezados); el catálogo de ese CI revisado (84 entradas: 77 apps y 7
  partes del sistema); tag; CI del tag (borrador con 185 archivos); los
  453 archivos que nombra el catálogo buscados en su release (125 en
  `v0.33.0`, 4 en `v0.32.1`, 16 en `v0.32.0`, 65 en `v0.31.0`, 214 en
  `v0.30.0`, 14 en `v0.29.0`, 15 en `v0.28.0`), con su tamaño, y 178 por
  SHA-256, sin un error; las dos imágenes dicen `v0.33.0`; notas
  (`docs/internal/RELEASE-0.33.0.md`), publicada, y `releases/latest`
  sirve su catálogo. Casi todas las apps se actualizan una vez: cada
  paquete trae ahora sus textos en los tres idiomas nuevos.

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
