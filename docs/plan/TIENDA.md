# P4OS — La Tienda

Una app del sistema que lee un catálogo publicado en GitHub y baja a la
tarjeta las apps y los juegos, con todo lo que cada uno trae: su `.so`, sus
packs de arte, su página del portal, sus idiomas y sus módulos. Con fuentes
de terceros: cualquiera puede publicar un catálogo en su propio repo y quien
quiera lo agrega en su placa, sabiendo lo que hace.

**Estado (2026-11-20): diseño, sin código.** Se trabaja en la rama `tienda`
(worktree `.claude/worktrees/tienda`, como se hizo con `ethernet`) y se
mergea a `main` cuando esté probada en la placa. Este documento es el plan;
lo que se vaya midiendo se anota acá, como en [ETHERNET.md](ETHERNET.md).

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
| Bajada por Wi-Fi | ~1,2 MB/s medido (`/api/fs/get`): Monster Hop, medio minuto; todos los `.so`, 10 s |
| Compatibilidad | `AOS_ABI_VERSION` 100 fijo, pero la tabla de símbolos crece con cada firmware: un `.so` nuevo puede pedir un símbolo que un firmware viejo no tiene, y hoy eso falla recién al cargarlo |
| Versiones | Las apps **no tienen versión**: el descriptor trae id, nombre, ícono, colores, banderas y orden |
| El ícono | Un blob AIC de hasta 512 B dentro del `.so`: entra en el catálogo, así la Tienda dibuja el ícono sin bajar la app |
| La red de seguridad | Modo seguro (BOOT apretado: sin apps de la tarjeta), vigilante de cuelgues con volcado, sha-256 en el HAL (`aos_hal_sha256_*`), `.part` + renombrar al escribir |
| Qué puede una app | Todo. Es código nativo dentro del firmware, sin protección de memoria (`docs/APPS-P4.md`). No hay permisos que mostrar ni que negar |
| Descomprimir | No encontré un inflate en lo que el firmware usa hoy (a confirmar en la fase 0). Los paquetes van sin comprimir: son chicos y no lo necesitan |

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
