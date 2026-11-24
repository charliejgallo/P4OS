# P4OS — Íconos nuevos

Plan para la versión después de la 0.33.0. **Es una de las dos estrellas de
esa versión, junto con el hub Zigbee/Thread** (`docs/internal/PEDIDOS-APPS-2026-11.md`,
G25). Pensado con el usuario el 2026-11-24; todavía no se empezó.

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
- Comprimidos con un formato simple (RGB565 + alfa con RLE o LZ4); unos
  10-20 KB los grandes y ~2 KB la miniatura.
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
