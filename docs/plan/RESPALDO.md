# Respaldos: plan

Pensado el 2026-11-21, con la Tienda a punto de entrar a `main`. Pedido del
usuario: la placa junta cada vez más cosas (ajustes, cuentas, apps, datos de
cada app) y no hay nada que las guarde.

**Dónde está (2026-11-21):** las cuatro etapas están escritas, en `main`
y en la placa (`v0.30.0-26-gac10384`); salieron con la 0.31.0, el
2026-11-22. Cada una tiene abajo lo que se hizo y lo que
dio en la placa.

**Lo que falta ver en la placa** (la lista está en `PRUEBAS-PLACA.md`):
restaurar un respaldo que cambie algo a la vista y uno simple; la copia de
antes de un firmware (en cuatro instalaciones no hubo cambios que la
pidieran); un pendrive de verdad; `sftp` hacia la placa; la copia semanal
al servidor saliendo sola; y la prueba con la flash borrada, que borra la
placa y espera la palabra del usuario.

## Etapa 1, hecha en la Mac (2026-11-21)

Escrita en la rama `respaldo`, que entró a `main` el mismo día. El documento para quien la usa es
`docs/BACKUP.md`. Lo que dio en la placa está más abajo, en "En la placa".

- **El servicio** (`components/aos_backup`): arma el archivo, lo lee, lo
  restaura. Un hilo propio hace el trabajo; las llamadas vuelven
  enseguida.
- **El archivo es un JSON, no un zip** (cambia D2): todo lo que lleva es
  texto chico, y un objeto que se puede abrir y leer vale más que un
  formato. No hizo falta un escritor de zip.
- **Los dos modos** (D3), como estaban pensados. El cifrado es AES-256-GCM
  con la clave derivada por PBKDF2-HMAC-SHA256, 40 000 vueltas (el número
  viaja en el archivo). Cuánto tarda eso en la placa está **sin medir**.
- **Qué es un secreto** se decide por el nombre de la preferencia:
  `pass`, `pwd`, `psk`, `token`, `secret`, `apikey`, `cred`, `auth`, la
  palabra `key` sola, y una lista (el código de bloqueo, los tokens de
  Claude, las claves privadas del portal, la clave del servidor SSH).
  Relevadas las del sistema (unas 110 con nombre fijo) y las 159 de las
  apps; **las que se arman con un número o un nombre al vuelo no se
  pudieron listar desde el código**: hay que mirarlas en la placa.
- **Lo que es de esta placa** (D4) no entra ni se restaura: los contadores
  de arranques, las marcas del volcado, el emparejamiento de Bluetooth,
  las sesiones del portal, lo de la batería y los contadores del día.
- **Restaurar** (D5): primero guarda lo que hay (`…-antes`, del mismo modo
  y con la misma frase), después escribe. Una frase equivocada o un
  respaldo de un P4OS más nuevo se rechazan antes de tocar nada. Una
  preferencia que no está en el respaldo queda como estaba.
- **Las apps**: el respaldo lleva la lista de los recibos de la Tienda con
  sus extras; después de restaurar, Ajustes y el portal ofrecen pedírselas
  a la Tienda. La cola de la Tienda pasó de 8 a 96 trabajos para eso.
- **Ajustes → Respaldo** y la página **Respaldo** del portal (bajar el
  archivo a la computadora, traer uno, restaurar, borrar).
- **Un error que apareció al escribirlo:** `aos_hal_pref_foreach` leía
  cada texto en 128 bytes, y uno más largo salía vacío (un certificado,
  la lista de fuentes de la Tienda). Nada usaba esa función todavía.
- **Probado:** `tests/backup_host`, 20 casos, con el servicio compilado
  para la Mac. En el simulador: un respaldo simple y uno cifrado por la
  API (el simple sin las dos credenciales de prueba, ninguno con su texto
  a la vista), la página de Ajustes, restaurar con los dos toques, el
  respaldo "de antes" que se arma solo, y la página del portal. Firmware
  `rev1_3`: 7 092 560 bytes, 24 KB más.

## En la placa (2026-11-21)

Instalado `v0.30.0-9-g40b9b5e` por OTA, con permiso. Los respaldos se
hicieron por la API del portal; del archivo se miraron los nombres de las
claves, no sus valores.

- **Un respaldo simple: 0,4 s.** 419 preferencias (350 números, 69
  textos), 8 afuera, las 7 apps de la Tienda, `menu.txt` y `modules.txt`:
  14 447 bytes. Los dos certificados del portal, de 624 y 660 caracteres,
  enteros (el arreglo de `aos_hal_pref_foreach`).
- **Uno cifrado: 3,8 a 4,1 s,** casi todo las 40 000 vueltas de PBKDF2.
  427 preferencias, 19 319 bytes. Abierto en la Mac con la misma frase
  (AES-GCM de otra librería, no la del firmware): las 427 adentro; con
  otra frase no abre. La frase fue una al azar que no se guardó, y el
  archivo se borró de la tarjeta.
- **RAM interna** mientras lo arma: 208,9 KB libres de mínima, contra
  209,3 KB en reposo.
- **Las 8 que el simple dejó afuera:** `wf_pass`, `ap_pass`, `ha_token`,
  `pt_pass`, `pt_token`, `pt_key`, `pt_cak`, `ssh_hk`. Las que tenían que
  ser.
- **Una que se le escapó: `sshc_seed`,** la clave privada del cliente SSH
  (la app SSH, una semilla Ed25519 en base64). Ninguna palabra del filtro
  está en ese nombre. Apareció buscando en el archivo los textos con forma
  de clave (base64 o hexadecimal, largos): los demás eran los dos
  certificados, que son públicos, y lo último que eligió el Programador.
  **Arreglado:** `sshc_seed` entra a la lista, se suma la palabra `priv`,
  y lo que D3 dejaba anotado: una preferencia que empieza con `sec_` es un
  secreto, para que una app lo pueda decir sin tocar el firmware. Con sus
  casos en `tests/backup_host`.
- **Lo que deja:** el filtro por nombre falló en la primera prueba de
  verdad. Va a volver a fallar con una app que no siga la regla, y por eso
  la pantalla dice "lo que parezca" y `docs/BACKUP.md` dice de abrir el
  archivo antes de sacarlo de la casa.
- **Con el arreglo instalado** (`v0.30.0-10-gcbd43ce`, el mismo día): un
  respaldo simple nuevo, 0,5 s, 418 preferencias y 9 afuera, 14 383
  bytes; ninguna de las nueve credenciales en el archivo, y lo que tiene
  que viajar (el nombre de la Wi-Fi, los certificados, `key_snd`) sigue
  ahí. El respaldo de la primera prueba, que llevaba la clave, se borró
  de la tarjeta.
- **Lo que viaja a la vista en un simple y es de la red de la casa:** el
  nombre de la Wi-Fi, las direcciones de los equipos que usan las apps
  (Home Assistant, Modbus, el escritorio remoto) y para qué dirección se
  hizo el certificado. No son credenciales; es para saber qué dice el
  archivo.

- **Restaurado desde la pantalla, por el usuario** (el mismo día): un
  respaldo cifrado hecho en Ajustes → Respaldo con la frase escrita dos
  veces, y restaurado medio minuto después con la frase y los dos toques.
  427 preferencias escritas; lo que había quedó antes en un
  `…-antes.p4bak`, cifrado con la misma frase (19 334 bytes); ninguna
  app para pedirle a la Tienda, porque la tarjeta las tenía todas. La
  placa siguió andando y quedó esperando el reinicio, que no hace sola.
  Visto por la API después; RAM interna libre, 209,2 KB.

- **Después del reinicio:** la placa arrancó normal con lo restaurado: en
  la red, con la hora, la Tienda leyendo su catálogo, 209,4 KB de RAM
  interna libres y los tres respaldos en la tarjeta.

**Falta para darla por terminada:** restaurar un respaldo simple y uno que
cambie algo a la vista (el de hoy escribió lo mismo que había). Y la
prueba de la tabla de etapas: con la flash borrada, volver a los ajustes
desde el archivo y que la Tienda ofrezca reinstalar. Esa borra la placa:
sólo cuando el usuario lo diga.

## Etapa 2, hecha en la Mac (2026-11-21)

Escrita en la rama `respaldo-2`, que entró a `main` el mismo día. Lo que
cambió respecto del plan está dicho en cada
punto.

- **Una copia por día, si algo cambió.** El plan pedía un contador de
  escrituras; quedó una **suma de las preferencias** (FNV-1a de cada una,
  sumadas, sin las que son de esta placa): no toca el HAL, sobrevive a un
  reinicio y no depende del orden en que la flash las lista. El tick del
  sistema mira una vez por minuto, pasados dos del arranque; el trabajo lo
  hace el hilo del módulo. Un día sin cambios queda anotado sin archivo.
  Sin reloj no hay copia: el día se sabe por la fecha.
- **Antes de un firmware,** con la misma condición: desde el portal
  (`PUT /api/ota`, que es también `tools/ota.sh`) y desde Ajustes →
  Actualización. Se hace y se espera antes de escribir la imagen; diez
  segundos como mucho, y la actualización sigue igual si no se pudo. Con
  la condición de "si cambió", diez instalaciones en una tarde de
  desarrollo no empujan afuera las copias de los días.
- **Quedan cinco** automáticas (las del día y las de antes de un firmware,
  juntas) y cinco de antes de restaurar. Las hechas a mano no se tocan.
- **Sin secretos mientras no haya frase** (la pregunta 3, como estaba
  recomendado). Ajustes → Respaldo → "Cifrarlas con una frase…": se
  escribe dos veces, en la placa. La placa **no guarda la frase**: guarda
  la sal y la clave que dio PBKDF2 (`bk_auto_key`, que el filtro deja
  afuera de un respaldo simple por su nombre), y cada copia lleva un IV
  nuevo. Restaurar una pide la frase, como cualquier cifrado, también en
  otra placa. Al poner la frase se hace enseguida la primera copia
  cifrada.
- **La oferta en una placa nueva:** un cartel sobre el inicio, a los seis
  segundos de arrancar, una sola vez: de cuándo es el respaldo más nuevo
  de la tarjeta, de qué placa y versión, cuánto lleva y si pide frase.
  "Ver respaldos" abre Ajustes → Respaldo; "Ahora no" lo deja ahí.
  **Qué es una placa nueva:** no conoce ninguna red Wi-Fi, nunca
  contestó el cartel y nunca hizo un respaldo. No restaura sola: abre la
  página, donde restaurar son dos toques.
- **Un error que encontró la prueba:** el número que ordena los respaldos
  de un mismo segundo se contaba por nombre, y uno borrado dejaba libre un
  número que volvía como "el más nuevo". Ahora es uno más que el último
  que haya, sea de la clase que sea.
- **El portal** muestra si las copias automáticas están prendidas y
  cifradas, y marca cada respaldo ("del día", "de antes de actualizar").
  Prenderlas, apagarlas y la frase: sólo en la placa (D7).
- **`tools/backup.sh`** (de la etapa 3, la parte que no pide firmware):
  pide un respaldo, lo baja a una carpeta de la computadora y lo saca de
  la tarjeta; con `FOLDERS` baja además carpetas de la tarjeta archivo por
  archivo, sin repetir las que ya tiene. `SEALED=1` pregunta la frase.
- **Probado:** `tests/backup_host`, 36 casos (16 nuevos). En el
  simulador: la copia del día hecha sola a los dos minutos de arrancar;
  la de antes de un firmware con una imagen de mentira (una vez, y
  ninguna al repetir); la frase puesta desde Ajustes con el teclado y la
  primera copia cifrada; el cartel de la placa nueva (`P4_SIM_SSID=`) y su
  salto a Ajustes; `tools/backup.sh` con una carpeta, dos veces.
  Firmware `rev1_3`: 7 103 376 bytes, 11 KB más que con la etapa 1.

**En la placa (2026-11-21, `v0.30.0-16-g021f337`, con las etapas 2 y
3):** instalado por OTA con permiso. A los 126 s de arrancar hizo sola la
copia del día: `…-auto.p4bak`, 418 preferencias, 9 afuera, 14 397 bytes.
RAM interna libre después: 209,2 KB, la de siempre.

**Falta:** la copia de antes de un firmware (la próxima instalación, si
algo cambió), la frase de las copias automáticas puesta en la pantalla, y
otro día. Y la prueba que termina la
etapa: `erase_flash`, grabar, y que la placa ofrezca el respaldo de la
tarjeta. Esa borra la placa: sólo cuando el usuario lo diga.

## Etapa 3, hecha en la Mac (2026-11-21)

En la misma rama, sobre la etapa 2.

- **Al pendrive, archivo por archivo:** `p4os-backup/backups` (los
  respaldos) y `p4os-backup/card/<carpeta>` (las carpetas elegidas). Antes
  hace un respaldo de ahora si algo cambió desde el último, con la misma
  regla de las copias del día.
- **Qué carpetas:** todas las de la raíz de la tarjeta menos las que la
  Tienda vuelve a bajar y la de los respaldos. Las grandes que vienen de
  una computadora (`photos`, `music`, `videos`, `maps`, `recordings`,
  `roms`) quedan apagadas hasta que la persona las prenda (la pregunta 2,
  como estaba recomendado). Lo elegido se guarda en `bk_dirs`, sólo lo que
  se cambió.
- **Cuándo se copia un archivo:** cuando el otro lado no lo tiene o lo
  tiene más viejo. Uno más nuevo del otro lado no se pisa nunca; con la
  misma fecha (una placa sin reloj los fecha a todos igual), decide el
  tamaño. Se escribe con otro nombre y se renombra: un pendrive sacado en
  el medio no deja medio archivo con nombre bueno.
- **De vuelta:** los respaldos y todas las carpetas que haya en el
  pendrive, a la tarjeta. Nunca a `/apps` ni a las otras que no son datos,
  aunque el pendrive las traiga.
- **Ajustes → Respaldo** suma "Pendrive" (copiar, traer con dos toques,
  detener) y un interruptor por carpeta, sólo con un pendrive montado. El
  portal suma "Copiar al pendrive"; traer se pide en la placa.
- **Probado:** `tests/backup_host`, 46 casos (10 de esta etapa): la copia
  entera, la segunda que no copia nada, un archivo cambiado y uno nuevo,
  una carpeta sumada y otra sacada, la vuelta a una tarjeta nueva con su
  restauración, un archivo más nuevo en la tarjeta que no se pisa, un
  pendrive sin copia. En el simulador (`P4_SIM_PENDRIVE=1`, una carpeta que
  hace de pendrive): la página de Ajustes y la copia pedida por el portal.
  Firmware `rev1_3`: 7 112 496 bytes, 9 KB más que con la etapa 2.

**Falta:** un pendrive de verdad en la placa: cuánto tarda con las
carpetas del usuario, qué pasa al sacarlo en el medio, y la prueba que
termina la etapa (una tarjeta nueva que queda con las notas y el inicio
de la anterior).

## Etapa 4, hecha en la Mac (2026-11-21)

Con las dos decisiones del usuario: el cliente SSH en el firmware, y el
lado servidor de SFTP en el SSH de la placa para probar contra el
simulador.

- **SFTP en el servidor SSH de la placa** (`aos_ssh_sftp.c`): `sftp`, `scp`
  y un administrador de archivos llegan a la tarjeta con las reglas y las
  claves de la consola. La raíz es la tarjeta y nada de arriba tiene
  nombre. Sirve por sí mismo (`docs/SSH.md`), y es contra lo que se prueba
  el cliente.
- **Un cliente SSH en el firmware** (`aos_sshc.c`), escrito aparte del de
  la app SSH y no compartido con ella: bloqueante, un canal, un pedido por
  vez, 700 líneas. **Cambia lo conversado** ("pasar el cliente de la app a
  un componente"): la app SSH quedó como estaba, con su transporte, y no
  pide un firmware nuevo; lo que comparten es la clave de la placa
  (`sshc_seed`). Unificarlos queda para cuando haga falta.
- **Sólo lo cifrado sale de la placa** (D3): suben los respaldos cifrados
  que el servidor no tiene, y no hay ninguno hasta que las copias
  automáticas tienen frase. Los simples no suben nunca.
- **Sin contraseñas:** la placa entra con su clave Ed25519. La línea para
  el `authorized_keys` del servidor está en el portal, para copiarla.
- **La clave del servidor se acepta en la placa** (D7): "Probar la
  conexión" muestra la huella que contestó y si dejó entrar; "Aceptar esta
  clave" (dos toques) la guarda. Después, un servidor con otra clave no
  recibe ni el intento de entrar. Cambiar la dirección la olvida.
- **Una vez por semana,** con la red arriba; si el servidor no está, se
  vuelve a probar en una hora. Y "Subir ahora". En el servidor no se borra
  nada (19 KB por semana).
- **El portal** muestra dónde está, la huella aceptada, la última copia y
  la clave de la placa; deja probar y subir, no elegir el servidor ni
  aceptar su clave.
- **Probado:** el servidor SFTP con el `sftp` y el `scp` de la Mac
  (OpenSSH 10.2) contra el simulador: listar, bajar 3 MB y subir 1,2 MB
  byte por byte, `scp -r`, renombrar, borrar, y nombres que intentan salir
  de la tarjeta. El cliente en `tests/sshc_host` (10 casos, contra dos
  simuladores: uno que deja entrar a su red y otro que pide clave): el
  intercambio, la huella, tres archivos byte por byte, otro servidor en su
  lugar rechazado por su clave. La lógica de qué sube en
  `tests/backup_host` (58 casos, 12 de esta etapa). Y de punta a punta en
  el simulador: una placa con la frase puesta por pantalla, la conexión
  probada, la clave aceptada con dos toques, dos respaldos cifrados en el
  servidor byte por byte, y el simple que no salió.
  Firmware `rev1_3`: 7 150 416 bytes, 38 KB más que con la etapa 3.

**En la placa (2026-11-21, `v0.30.0-21-gd2bc7d3`):** instalado por OTA
con permiso, confirmado. RAM interna libre: 209,1 KB, la de siempre. La
instalación no dejó copia "de antes de actualizar", y está bien: nada
había cambiado desde la copia del día. La placa ya tiene su línea de clave
en el portal; el servidor está sin elegir.

**Antes de autorizar la clave de la placa en un servidor:** el valor de
`sshc_seed` de esta placa quedó a la vista en el registro de una sesión de
trabajo el 2026-11-21 (un error de esa sesión, al revisar un respaldo).
Esa clave hay que cambiarla por una nueva antes de ponerla en el
`authorized_keys` de nadie. Para eso, el mismo día: Ajustes → Respaldo →
"Hacer una clave nueva" (dos toques), que reemplaza `sshc_seed`; probado
en el simulador y en `tests/backup_host` (59 casos). Instalado el mismo
día (`v0.30.0-23-g2cb14b8`, confirmado, 209,1 KB de RAM interna libres);
falta que el usuario lo toque en la placa.

**El NAS del usuario, mirado desde la Mac (2026-11-21, con su permiso):**
contesta por SSH (un OpenSSH) y ofrece clave pública o contraseña; ninguna
clave de la Mac está autorizada para su usuario, así que la carpeta no se
pudo ver. La contraseña no la escribe una sesión: el paso de autorizar una
clave en el NAS es del usuario.

**El mismo día, más tarde:** el usuario hizo la clave nueva en la placa y
autorizó en el NAS, con comandos que corrió él, la clave de la Mac y la de
la placa (esta limitada a SFTP con `restrict,command="internal-sftp"`).
Lo que hubo en el camino: `ssh-copy-id` no sirvió (el NAS corta en su
prueba con la clave; entrando sólo con contraseña anduvo); la carpeta
personal del usuario no era suya y `~/.ssh` hubo que crearla con `sudo`; y
la carpeta de los respaldos, creada desde el NAS, es de root y el usuario
no la puede escribir: hizo falta un `chown`.

**La primera copia a un servidor de verdad** (2026-11-21, la placa con
`v0.30.0-23-g2cb14b8`, un NAS con OpenSSH 10.2): desde Ajustes, el
servidor y la carpeta cargados por pantalla, "Probar la conexión" (la
huella del NAS, y la placa adentro con su clave nueva, limitada a SFTP),
"Aceptar esta clave", la frase de las copias, y "Subir ahora": **los tres
respaldos cifrados de la tarjeta en el NAS, byte por byte** (SHA-256 de
cada uno, leído en el NAS y en la placa), en `<carpeta>/<nombre de la
placa>/`; los dos simples no salieron; ningún archivo a medias. La copia
semanal quedó prendida. RAM interna libre después: 209,0 KB. Es también
la primera vez que el cliente del firmware habla con un OpenSSH.

**Lo que dijo el usuario al probarlo:** antes de poner la frase tocó
"Subir ahora" varias veces "y no pasaba nada": el motivo salía en la fila
Estado, que está dos pantallas más arriba. Arreglado el mismo día: un
aviso en naranja al lado del botón mientras falte la frase, un mensaje al
tocarlo, y un mensaje con cómo terminó cada trabajo del servidor o del
pendrive. Y dos errores que aparecieron al probar eso en el simulador: el
archivo de preferencias del simulador se pisaba con dos hilos escribiendo
a la vez, y en el firmware dos hilos que no encontraban la clave de la
placa hacían una cada uno (el portal muestra su línea mientras un trabajo
entra al servidor): ahora la hace uno solo. Instalado el mismo día
(`v0.30.0-26-gac10384`, confirmado, 209,1 KB de RAM interna libres, con
el servidor, su clave y la copia semanal como estaban).

**Falta:** SFTP hacia la placa con un cliente de verdad, y ver la copia
semanal salir sola. Sin probar: RSA o ECDSA como clave del servidor, y
ChaCha20 del lado cliente (negocia AES-GCM primero).

## Qué resuelve

Hoy, si se borra la flash, se rompe la tarjeta o una actualización deja la
placa en mal estado, lo que se pierde hay que rehacerlo a mano: las redes y
sus contraseñas, el código de bloqueo, la contraseña del portal, las claves
SSH, los tokens de Home Assistant y MQTT, las fuentes de la Tienda, el
orden del inicio, los ajustes y los puntajes de cada app, las notas.

Con la Tienda, además, una placa nueva se arma sola menos por eso: las apps
se vuelven a bajar, la configuración no.

El objetivo: **grabar, restaurar y dejar que la Tienda reinstale**. Tres
pasos para volver a tener la placa como estaba.

## Lo que hay hoy, leído del repo

- **El estado vive en dos lugares.**
  - **La flash:** la partición `prefs` (256 KB, 8064 entradas; la placa
    del usuario usa unas 600) con las preferencias del sistema y de las
    apps, y `nvs` con los emparejamientos de Bluetooth y lo mínimo para
    volver a una versión vieja (`docs/BUILDING.md`).
  - **La tarjeta:** los datos de cada app (`/notas`, `/redes`, `/python`,
    `/lua`, las partidas y registros que cada una escribe), `menu.txt` (el
    inicio), `modules.txt` (lo cableado al header), `/photos`, `/music`,
    `/videos`, `/maps`; y lo que se vuelve a bajar: `/apps`, `/web`,
    `/lang`, `/fonts`, `/firmware`.
- **La pieza base existe:** `aos_hal_pref_foreach()` recorre todas las
  preferencias, y su comentario dice para qué: "for the portal's backup".
  Vino de AmoledOS, cuyo portal tenía `GET`/`POST /api/respaldo`
  (`components/aos_web/aos_web.c`): todas las preferencias como JSON, las
  de Wi-Fi sólo si se pedían, una lista de claves "de este reloj" que no se
  restauran en otro, y `menu.txt` en el mismo archivo. El portal del P4 no
  lo ofrece.
- **Sacar archivos ya se puede:** el portal baja y sube cualquier archivo
  de la tarjeta (`/api/fs/get`, `/api/fs/put`), el USB tiene modo disco, y
  la placa es host USB (un pendrive), cliente HTTP(S) y cliente SSH.
- **La Tienda ya sabe qué hay instalado:** un recibo por app en
  `apps/.store/`, y las fuentes en las preferencias (`store_srcs`).
- **La flash no está cifrada** (`docs/SECURITY.md`): las contraseñas están
  en claro adentro de la placa. Un respaldo las saca de ahí.

## Decisiones

**D1. Tres clases de cosas, tres tratos.**

| Qué | Tamaño | En el respaldo |
|---|---|---|
| Las preferencias | decenas de KB | siempre, enteras |
| Los datos del usuario en la tarjeta | de KB a GB | por carpetas elegidas; las chicas por defecto |
| Lo que se vuelve a bajar (apps, packs, emojis, idiomas) | 170 MB | **no se copia**: va la lista, y la Tienda reinstala |

**D2. Un archivo, un formato.** (En la etapa 1 quedó como un JSON: ver
arriba.) `p4os-<nombre>-<fecha>.p4bak`: un zip sin
compresión (el lector ya está en `aos_store.c`) con `backup.json` (versión
del formato, de qué firmware y de qué placa salió, la fecha), `prefs.json`,
`store.json` (apps instaladas con sus extras, y las fuentes), y los
archivos chicos de la tarjeta tal cual (`menu.txt`, `modules.txt`). Las
carpetas grandes no entran al archivo: se copian aparte, archivo por
archivo, para poder retomarlas.

**D3. Los secretos van cifrados o no van.** Dos modos:

- **Completo:** todas las preferencias, cifradas con una frase que elige la
  persona (AES-256-GCM, la clave derivada con PBKDF2; mbedTLS ya está en el
  firmware). Es el único modo para lo que sale de la placa por la red.
- **Sin secretos:** sin frase. Deja afuera lo que parezca una credencial
  (`*pass*`, `*token*`, `*key*`, `*psk*`, las claves SSH, el código de
  bloqueo) y lo dice en la pantalla, porque es un filtro por nombre y una
  app puede guardar un secreto con otro nombre.

Para cerrar ese agujero, una app que guarda un secreto lo declara: el
nombre de la preferencia empieza con `sec_` (hecho el 2026-11-21, y en el
handoff de apps).

**D4. Lo que es de esta placa no se restaura en otra:** los emparejamientos
de Bluetooth (están en `nvs` y dependen de claves del otro lado: se vuelve
a emparejar), el certificado HTTPS del portal, la calibración y los
contadores. Una lista en el código, como la de AmoledOS.

**D5. Restaurar es explícito y se puede deshacer.** Antes de restaurar se
guarda un respaldo de lo que hay. Las preferencias se escriben de a una, y
al final se ofrece reiniciar (la mayoría se lee al arrancar). Un respaldo
de un firmware más nuevo que el que corre se rechaza.

**D6. Sin nubes propietarias en el firmware.** Drive, iCloud y Dropbox
piden flujos de autorización largos y dejarían un token de la cuenta entera
en una flash sin cifrar. Lo que sí: un destino **del usuario** por SFTP o
WebDAV (un NAS, una Raspberry, Nextcloud), o un script en la computadora
que deja el archivo en una carpeta que ya se sincroniza.

**D7. Nunca por su cuenta hacia afuera.** Las copias automáticas son a la
tarjeta. Mandar algo fuera de la placa pide haber configurado el destino y
la frase, en la placa.

## Dónde se guarda

| Destino | Protege de | Cómo |
|---|---|---|
| **La tarjeta** (`/backups`) | borrar la flash, una actualización mala, un ajuste roto | automático; las últimas cinco |
| **Una computadora, por el portal** | además, de perder la tarjeta | "Bajar respaldo" y "Restaurar"; `tools/backup.sh` para programarlo |
| **Un pendrive en el host USB** | lo mismo, sin red | "Copiar al pendrive", con las carpetas elegidas |
| **Un servidor del usuario** (SFTP o WebDAV) | lo mismo, sin tocar nada | programado, cifrado |

## Cada cuánto

- **En la tarjeta:** una vez por día si alguna preferencia cambió desde la
  última copia (un contador de escrituras alcanza para saberlo), y siempre
  **antes de instalar un firmware** y antes de restaurar. Se guardan cinco.
- **Fuera de la placa:** semanal cuando hay un destino configurado, y a
  mano cuando se quiera. La pantalla dice de cuándo es la última y avisa
  si pasó un mes.

## Desde dónde

- **Ajustes → Respaldo** (página nueva): "Respaldar ahora", la fecha y el
  tamaño de la última, qué carpetas entran, el destino, y "Restaurar" con
  la lista de las que hay en la tarjeta o en el pendrive.
- **El portal:** bajar el archivo y subirlo para restaurar.
- **El primer arranque de una placa recién grabada:** si la tarjeta trae un
  respaldo, ofrecer restaurarlo antes de pedir el Wi-Fi a mano.
- **La Tienda:** después de restaurar, una cola con las apps y los extras
  de la lista, que la persona acepta o recorta.

## Riesgos, y qué hay que mirar antes

1. **Un secreto con un nombre que el filtro no ve** (D3): recorrer las
   claves que escriben hoy el sistema y las 60 apps antes de fijar la
   regla.
2. **Restaurar a medias:** un corte de energía mientras se escriben 600
   preferencias. Escribir primero a un lado y cambiar al final, o aceptar
   que la restauración se repite entera; medir cuánto tarda.
3. **Restaurar en otra placa o en otra versión:** preferencias que cambian
   de significado entre versiones. El archivo dice de qué firmware salió;
   decidir qué se hace con uno viejo (aceptarlo: las claves que ya no
   existen se ignoran).
4. **El tamaño de las carpetas de datos:** medir en la tarjeta del usuario
   cuánto pesan `/notas`, las partidas y los registros, para elegir los
   valores por defecto.
5. **La frase olvidada:** un respaldo completo sin su frase no sirve.
   Decirlo en la pantalla al elegirla; no hay recuperación.
6. **La RAM interna:** el cifrado y el zip corren en un hilo con pila y
   búferes en PSRAM, como el resto (`docs/MEMORY.md`).

## Etapas

Cada una termina con algo que se puede probar.

| Etapa | Qué | Dónde | Terminada cuando |
|---|---|---|---|
| **1. El archivo** | El formato, los dos modos, bajar y restaurar por el portal, "Respaldar ahora" en Ajustes; la lista de apps y fuentes | firmware, portal | Una placa con la flash borrada vuelve a sus ajustes desde el archivo, y la Tienda ofrece reinstalar |
| **2. Solo** | La copia diaria en la tarjeta y antes de cada firmware, las cinco últimas, la oferta al primer arranque | firmware | Tras un `erase_flash` y una grabación, la placa ofrece el respaldo de la tarjeta |
| **3. Los datos** | Carpetas elegidas a un pendrive; `tools/backup.sh` desde la computadora (este se puede escribir hoy, sin firmware) | firmware, herramientas | Una tarjeta nueva queda con las notas y el inicio de la anterior |
| **4. Por la red** | Un destino SFTP o WebDAV, programado, siempre cifrado | firmware | Una semana de copias a un servidor de prueba, sin tocar nada |

Para probar, como siempre: servidores falsos en la Mac (`tools/fake_*.py`),
nunca los equipos ni las cuentas del usuario, y en la placa sólo cuando él
lo diga.

## Preguntas abiertas

1. **¿SFTP o WebDAV primero?** Resuelto el 2026-11-21: **SFTP**. El
   usuario tiene un NAS en su red que atiende por Samba y por SSH; el
   cliente SSH ya está en el firmware (`components/aos_ssh`) y no hay
   cliente SMB, que sería una pieza nueva y grande. WebDAV queda para
   después, si alguien lo pide. La etapa 4 se prueba primero contra un
   servidor SSH falso en la Mac; contra el NAS de verdad, sólo cuando el
   usuario lo diga, con una cuenta y una carpeta que él arme para eso.
2. **¿Las fotos, la música y los mapas entran?** Recomendado: no por
   defecto; suelen venir de una computadora y pesan gigas.
3. **¿La copia diaria en la tarjeta lleva secretos?** La tarjeta se saca.
   Recomendado: sin secretos mientras no haya frase; completa y cifrada
   apenas la persona elige una.
4. **¿Un respaldo se puede restaurar por partes** (sólo las redes, sólo una
   app)? Recomendado: en la primera versión, todo o nada, más la lista de
   apps aparte.
