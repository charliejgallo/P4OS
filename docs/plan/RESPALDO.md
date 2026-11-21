# Respaldos: plan

Pensado el 2026-11-21, con la Tienda a punto de entrar a `main`. Pedido del
usuario: la placa junta cada vez más cosas (ajustes, cuentas, apps, datos de
cada app) y no hay nada que las guarde.

**Dónde está (2026-11-21):** la etapa 1 está escrita, probada en la placa
y en `main`; sale con la 0.31.0, que espera a las apps nuevas de la
Tienda. Las etapas 2 a 4 (la copia diaria, el pendrive, el servidor
propio) no están empezadas.

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
