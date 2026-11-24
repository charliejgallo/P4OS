# tools/icons — los íconos HD

La etapa "Estilo" de `docs/plan/ICONOS.md`: tres estilos de muestra para
elegir, y el método para hacer los ~95 íconos de la producción con el que
se elija. Todo se dibuja por código acá (SVG escrito a mano o modelos de
Blender hechos por script); no hay nada bajado de internet.

## Lo que hay

| Archivo | Qué hace |
|---|---|
| `common.py` | Las apps de la muestra, los tamaños, la forma del ícono, SVG → PNG |
| `style_a_plano.py` | Estilo A, plano con degradé (SVG) |
| `style_b_chubby.py` | Estilo B, 3D de juguete (Blender 3.3, `-b`) |
| `style_b_post.py` | Estilo B: achica los renders 4× a 120, 108 y 26 |
| `style_c_linea.py` | Estilo C, línea duotono sobre fondo oscuro (SVG) |
| `sim_shots.py` | Arma un pack por estilo y saca capturas del simulador (**abre una ventana**) |
| `sheets.py` | Las láminas para comparar y la tabla de pesos |
| `peek.py` | Una lámina rápida de un estilo, para mirar mientras se dibuja |
| `mkpack.py` | El pack `hd.pak` (de la sesión de firmware) |

Salida en `muestra/<estilo>/<id>_<tamaño>.png` (y `muestra/<estilo>/svg/`
para A y C), láminas en `muestra/laminas/`, pesos en `muestra/pesos.md`.

```bash
python3 tools/icons/style_a_plano.py
python3 tools/icons/style_c_linea.py
/Applications/Blender.app/Contents/MacOS/Blender -b -P tools/icons/style_b_chubby.py -- --samples 128
python3 tools/icons/style_b_post.py
python3 tools/icons/sim_shots.py          # opcional: abre el simulador
python3 tools/icons/sheets.py
```

Necesita `rsvg-convert` (Homebrew, `librsvg`) y Pillow. Blender tarda
~1 minuto por los 9 íconos en la GPU de la Mac (128 muestras, 480 px).

## Los tamaños y la forma

- **120** (inicio de pie, `AOS_UI_ICON`), **108** (acostada,
  `AOS_UI_ICON_LAND`), **26** (miniatura de carpeta: `folder_icon()` en
  `aos_home.c`, carpeta de 120, pad 15, separación 5, tres por fila).
- El 120 y el 108 son el mismo dibujo. **El 26 es otro dibujo**, de la
  misma idea: la silueta al ~70 % del cuadrado, trazos de 2 px o más (9
  unidades de 120), sin sombras ni brillos, nada de menos de 1 px.
- La forma es el cuadrado redondeado del firmware (esquina 22,5 % del lado),
  **dentro de la imagen**: la placa no dibuja fondo atrás (`mkpack.py`).
  El resto de la imagen es transparente.

## Estilo A — plano con degradé

SVG en un `viewBox` de 120. La receta (`TILE`, `SHADOW`, `WHITE` en el
script):

- **Fondo:** degradé vertical de dos tonos del mismo color, el de arriba
  ~15 % más claro; encima un brillo blanco del 22 % al 0 % en la mitad de
  arriba y un filete de luz de 1,2 unidades.
- **Dibujo:** blanco a gris muy claro (`#FFFFFF → #E4E7EE`) salvo que la
  idea pida color; a lo sumo tres colores además del fondo, cada uno con su
  degradé.
- **Sombra:** desenfoque 2,6, 2,2 hacia abajo, negro al 30 %, debajo del
  dibujo.
- **Esquinas redondeadas** en formas rectas: relleno más un trazo del mismo
  color con `stroke-linejoin="round"` (así los dientes del engranaje y la
  ficha no quedan filosos).
- **Miniatura:** lo mismo sin sombra ni brillo, con las formas más gordas.

## Estilo B — 3D de juguete

`style_b_chubby.py`, en la línea de `apps/tanques/tools/blender/tanques_art.py`.
La escena, igual para todos:

- **La ficha:** el cuadrado redondeado extruido 0,12 y biselado 0,035, de
  plástico (rugosidad 0,42, barniz 0,3), con un degradé vertical de los dos
  colores de la app hecho con nodos (`mat_grad`). `stripes` le pone franjas
  (la cancha del Metegol).
- **El juguete:** un solo objeto, gordito, inclinado 10-25° para que se vea
  el volumen, levantado de la ficha para que proyecte sombra. Las piezas:
  `slab` (un contorno 2D extruido y biselado: engranaje, ficha serie,
  montañas), `rbox` (caja redondeada), `ball`, `cyl`, `tube` (cable),
  metaballs (la nube).
- **Luz:** un sol suave de arriba a la izquierda (disco de 35°, sombras
  borrosas), otro tenue de abajo a la derecha para el borde, mundo blanco al
  0,5. Vista "Standard" (Filmic lava los colores). Cámara ortográfica,
  derecho hacia abajo, la ficha llena el cuadro justo.
- **Render** a 480 px (y 104 la miniatura) y achicado 4× con alfa
  premultiplicado (`style_b_post.py`), si no los bordes transparentes
  oscurecen el filo de la ficha. La miniatura lleva un poco de máscara de
  enfoque.
- **Miniatura:** el juguete más grande (×1,1-1,25) y sin las piezas chicas
  (los nueve pines pasan a tres, sin lápiz, sin rayos del sol, un jugador
  por barra).

## Estilo C — línea duotono sobre fondo oscuro

SVG. Todas las fichas son el mismo casi negro (`#262932 → #0D0E12`), con un
filete y un halo muy tenue del color de la app; el color vive en la línea:

- **Dos colores por app** (`PAIRS`): uno claro arriba a la izquierda y uno
  más profundo abajo a la derecha, de tonos vecinos.
- **La línea:** 6,5 unidades de 120, puntas y uniones redondas, con el
  degradé; debajo, la misma línea desenfocada (3,2) al 65 % para que brille.
  Lo que queda atrás se tapa con el color de la ficha (`TILE_C`).
- **Miniatura:** una línea de 1,4 px con brillo no se lee: es la silueta
  rellena con el mismo degradé, con los detalles calados en el color de la
  ficha.

## Lo que pesa

Promedio de los 9 íconos (`muestra/pesos.md` tiene cada uno). QOI es lo
que guarda el pack.

| Estilo | PNG 120 | QOI 120 | PNG 108 | QOI 108 | PNG 26 | QOI 26 |
|---|---|---|---|---|---|---|
| A · Plano con degradé | 6,8 KB | 7,3 KB | 6,3 KB | 6,7 KB | 0,8 KB | 0,7 KB |
| B · 3D de juguete | 16,9 KB | 20,4 KB | 14,1 KB | 17,0 KB | 1,7 KB | 1,9 KB |
| C · Línea duotono | 12,8 KB | 16,4 KB | 11,0 KB | 13,7 KB | 1,1 KB | 1,2 KB |

Para ~95 íconos con los tres tamaños, el pack quedaría en **~1,4 MB (A),
~3,7 MB (B) y ~3 MB (C)**. El plan contaba con 1-2 MB: con B o C conviene
no guardar el 108 (la placa achica el 120, `mkpack.py` lo permite), que lo
baja a ~2,1 MB y ~1,7 MB. Lo que más pesa en B es el ruido fino del render y
los degradés suaves; en C, el brillo desenfocado.

## La muestra

Seis íconos que eligió el usuario: Ajustes (`aos.settings`), Terminal
(`aos.serial`), Clima (`aos.clima`), Notas (`aos.notas`), Metegol
(`demo.metegol`) y Tienda (`aos.store`), cada uno conservando la idea del
ícono de hoy. Más tres (Reloj, Calculadora, Fotos) sólo para que la carpeta
de las capturas tenga nueve miniaturas de verdad: no entran en la elección.

Las capturas de contexto son del simulador con el pack de cada estilo
(`sim_shots.py`): como una app aparece una sola vez en `menu.txt`, la
grilla y la carpeta son dos corridas.
