# P4OS — Íconos nuevos

Plan para la versión después de la 0.33.0. **Es una de las dos estrellas de
esa versión, junto con el hub Zigbee/Thread** (`docs/internal/PEDIDOS-APPS-2026-11.md`,
G25). Pensado con el usuario el 2026-11-23; **la parte del firmware se hizo el
2026-11-24** (abajo, "Lo hecho"). **Estilo elegido (2026-11-24): el B, 3D de
juguete hecho en Blender** (`tools/icons/`, la muestra en la rama
`claude/iconos-estilo-365bbb`), y **packs alternativos**: el C (línea
duotono sobre fondo oscuro) le gustó mucho al usuario y va a ser el segundo
pack. Falta la producción.

## Por qué

Con más de 80 apps, los íconos del inicio se ven simples:

- **Las apps del sistema** son un círculo con degradé y un glifo de un color
  (`aos_icon.c`, la fuente de símbolos a 72 px): prolijo, plano, todos
  iguales.
- **Las apps de la tarjeta** traen su ícono como AIC (`aos_icon_ops.h`): una
  lista de hasta 256 bytes de rectángulos redondeados, anillos, arcos y un
  degradé. Sin curvas libres, sombras, brillos ni texturas.

Son vectoriales y nítidos: el límite es lo que pueden dibujar, no la
resolución. AIC se hizo para que una app trajera su ícono sin firmware y
pesando casi nada (`../ESP32S3_AmoledOS/docs/ICONS.md`); eso sigue valiendo
como respaldo.

## Lo que se decidió

**Íconos dibujados en la Mac y guardados como imágenes**, en un **pack de
íconos que la Tienda instala como parte del sistema** (como los idiomas y
los emojis), un ícono por id de app:

- Origen en SVG, o en Blender si se elige un estilo 3D (como Tanques o
  Monster Hop). El pipeline los renderiza a los tamaños justos.
- **Tres tamaños por ícono:** 120 px (inicio de pie, `AOS_UI_ICON`), 108
  (acostada, `AOS_UI_ICON_LAND`) y **26 (la miniatura de las carpetas**,
  9 por carpeta en 3×3, `folder_icon()` en `aos_home.c`), esta última hecha a
  propósito y simplificada si hace falta (menos detalle, trazos más gruesos),
  no achicada en la placa. Más un PNG para el portal y la Tienda.
- Guardados como QOI (RGBA sin pérdida, el decodificador de los emojis):
  unos 7-10 KB los grandes y 0,2-1,5 KB la miniatura, medido con los de
  prueba.
- **Ninguna app se actualiza** (los íconos no van en cada `.so`): no hay
  otra actualización masiva.
- **Respaldo:** sin el pack, o una app sin ícono en él (una de terceros),
  se dibuja su AIC o el glifo de siempre.
- Abre la puerta a **temas de íconos** elegibles en Ajustes (plano, 3D,
  monocromo).

Descartado: ampliar AIC (mejora poco, se sigue escribiendo a mano) y SVG
dibujado en la placa con ThorVG (~400-600 KB de flash de los ~1,1 MB libres,
y el costo de dibujar).

## Memoria y tiempo

- Una página del inicio: 24 íconos, ~1 MB de PSRAM; sólo la página que se ve.
- Miniaturas: una página con 6 carpetas llenas son 54, ~110 KB; todas las
  apps en carpetas, ~190 KB. Cada miniatura pasa a ser un solo objeto de
  LVGL (hoy es el AIC entero: un ícono de 7 figuras son 7 objetos), así que
  dibujar carpetas se vuelve más barato.
- Abrir una carpeta carga 9-12 íconos de 120 px: milisegundos.
- `/data/apps.cache` guarda hoy el blob AIC de cada app; con el pack, la
  imagen sale del pack por id, no del `.so`.

## El trabajo

| Etapa | Qué | Tamaño |
|---|---|---|
| Firmware | leer el pack, mostrar imágenes en el inicio, las carpetas, la Tienda y el portal; caché; respaldo al AIC; la parte del sistema en `store/system.json`; medir memoria y arranque | 1 sesión (firmware) |
| Estilo | 5-6 íconos de muestra en 2-3 estilos para que el usuario elija | 1 sesión corta, con el usuario |
| Producción | ~95 íconos (~60 de la tarjeta, ~25 del sistema, carpetas, partes del sistema), una sola familia | 1-2 sesiones, repartibles por grupos |
| Release | el pack como parte del sistema (~1-2 MB) | dentro de un release |

Lo que más pesa es el diseño coherente, no el firmware.

## Lo hecho (firmware, 2026-11-24)

- **El pack:** `<tarjeta>/icons/<nombre>.pak`, uno por estilo
  (`juguete.pak` el B, `linea.pak` el C; `hd.pak` fue el de prueba). Un índice (id de la app, tamaño,
  dónde está) y las imágenes en QOI; el formato está en
  `components/aos_ui/include/aos_icon_pack.h`. El índice se lee a PSRAM y
  se vuelve a leer si el archivo cambia (se mira cada 2 s como mucho), así
  que la Tienda puede reemplazarlo sin reiniciar.
- **El dibujo:** `aos_icon_create()` y `aos_icon_create_ops()` (`aos_icon.c`)
  preguntan primero al pack por el id; si tiene imagen, el ícono es un
  `lv_image` con su propia imagen decodificada en PSRAM (ARGB8888, 57 KB uno
  de 120 px, 2,7 KB una miniatura), que se libera con el objeto. Si no, se
  dibuja como siempre (AIC o glifo). Eso cubre el inicio, el dock, las
  carpetas y sus miniaturas, la Tienda (también las apps no instaladas, por
  su id) y la pantalla de "pronto".
- **Tamaños:** se usa el del pack igual o más grande más cercano y se
  achica; lo justo es tener 120, 108 y 26.
- **Varios packs:** la preferencia `icon_pack` elige cuál: vacía, el primero
  por nombre que haya en la tarjeta (así un pack recién bajado se ve sin
  elegir nada); `off`, ninguno; si no, el nombre. Si el elegido no está, el
  primero que haya. **Ajustes → Fondo de pantalla → Íconos** los lista ("Los
  de siempre" y cada pack, con su nombre traducido si es uno de los
  publicados: `pack_title()` en `aos_app_settings.c`) y el inicio cambia en
  el momento. Cuando la Tienda instala algo en `icons/`, el inicio se arma
  de nuevo (`aos_ui_request_icons()`).
- **La Tienda** acepta `icons/<nombre>.pak` como parte del sistema
  (`sys_allowed()` en `aos_store.c`, `SYS_PATH` en `tools/gen_store.py`).
- **La herramienta:** `tools/icons/mkpack.py <carpeta> [salida]` arma el
  pack con `<id>.png` (el original, cuadrado, de 120 px o más, que se achica
  a los tamaños que falten) y `<id>@<N>.png` (uno hecho para ese tamaño, como
  la miniatura de 26). `--list` muestra lo que tiene un pack. Las imágenes
  son RGBA con transparencia afuera de la forma del ícono: la placa no
  dibuja el fondo redondeado, va en la imagen.
- **Probado en el simulador** con un pack de prueba de dos íconos (Reloj y
  Calculadora, y los mismos con ids del Taller para ver las miniaturas):
  inicio, dock, carpeta abierta y miniaturas, la de 26 hecha a mano y la
  achicada por la herramienta. Para probar sin tocar el `sim_fs` de las
  otras sesiones, el simulador corre desde otra carpeta con un `sim_fs` de
  enlaces a `sim/sim_fs` y su propio `icons/`.

### Medido en la placa (2026-11-24)

Con un pack de prueba de las 112 apps de la placa del usuario (iniciales
sobre el color de cada app, 1,2 MB, 336 imágenes):

- **El inicio arma todas sus páginas y las carpetas al arrancar**, no sólo
  la que se ve: 117 imágenes (23 primero y 94 cuando se cargan las apps de
  la tarjeta), **unos 2,5 MB de PSRAM** (de 15,2 MB libres quedaron 13,4).
- **Tiempo:** abriendo el pack para cada ícono, 485 ms en total (~4 ms cada
  uno, casi todo abrir el archivo en la tarjeta). Dejándolo abierto mientras
  se arma una tanda (se cierra medio segundo después): **211 ms** (48 + 163),
  ~1,8 ms cada uno. El registro lo dice en cada tanda: `icon: HD icons: N
  pictures in X ms, Y KB`.
- No hace falta una caché por ahora. Si la PSRAM aprieta, guardar las
  imágenes en RGB565A8 (3 bytes por punto en vez de 4) o armar sólo la
  página que se ve.

## Las variantes (pedido del usuario, 2026-11-24)

En la producción, **dos o tres variantes de cada ícono** para que el
usuario elija una por app: una hoja con las variantes lado a lado (en el
tamaño real, 120 y 26, sobre el fondo de pantalla) y lo que elige queda
anotado; el pack se arma con las elegidas.

### Lo que falta

- **El estilo y los dibujos** (las etapas de arriba).
- **El portal** sigue dibujando el AIC; mostraría el PNG si la placa sirve
  los íconos del pack (por ejemplo `/api/icon/<id>.png`).
- **Un pack nuevo publicado** suma su nombre a `pack_title()` (con sus
  traducciones) y su parte del sistema.
- **La release:** en `store/system.json`, una parte por pack (los textos,
  el glifo y los colores a gusto):

  ```json
  {"file": "sys-icons-juguete", "files": [{"path": "icons/juguete.pak", "asset": "icons_juguete.pak"}], ...}
  ```

  y el pack subido a la release publicada antes del tag, como los demás.
