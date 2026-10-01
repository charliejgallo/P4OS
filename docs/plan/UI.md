# P4OS — Interfaz: un SO "tipo iPhone" en 720×1280

AmoledOS se pensó para una muñeca: una app a la vez, esferas, menú en lista. P4OS
se piensa como un teléfono apoyado en el banco: pantalla de inicio con íconos
en páginas, carpetas, dock, barra de estado, centro de control, notificaciones,
multitarea y **vertical u horizontal a elección**.

## Escala

- La P4 tiene **~294 ppi** y el reloj ~322 ppi: **un píxel mide casi lo mismo en
  las dos**. Un botón de 60 px del reloj es igual de grande (con el dedo) en la
  P4. Las apps portadas **no se agrandan**: se re-maquetan para usar más espacio.
- Unidad de diseño: píxel real. Referencias del estilo iOS: íconos de ~120 px,
  texto normal de 28 px, títulos de 40-48 px, mínimo tocable de 88 px.
- Fuentes: además de las de AmoledOS (14-48), hacen falta 24, 32 y 64/96 (reloj
  grande, protector de pantalla). Todas generadas con `gen_fonts.py`, Latin-1.

## Orientación

- Global: **Vertical** (720×1280, nativo) u **Horizontal** (1280×720). No hay IMU,
  así que no hay "automático". Se cambia desde el Centro de control (botón de
  rotación, como el candado de iOS pero al revés: elige), desde Ajustes →
  Pantalla y con un gesto largo en el inicio.
- Cada app declara qué orientaciones acepta
  (`AOS_APP_ORIENT_PORTRAIT | _LANDSCAPE`). Si la actual no le sirve, **la
  pantalla gira sólo mientras está abierta** (Doom siempre horizontal, un juego
  retro siempre vertical) y vuelve al salir.
- Al girar, la app recibe `resize(root, w, h)`. Si no lo implementa, el SO la
  destruye y la vuelve a crear (las apps de AmoledOS ya guardan su estado en
  variables estáticas, así que esto les sirve sin cambios).
- Cómo se implementa (decisión en la Fase 2, prueba 6): LVGL dibuja en
  coordenadas lógicas y el flush **rota con el PPA** al framebuffer DPI (el
  PPA rota en 90/180/270, escala y convierte formato en un solo paso).

## Anatomía de la pantalla

```
Vertical 720×1280                         Horizontal 1280×720
┌──────────────────────────┐              ┌──────────────────────────────────────────┐
│ 14:32   ●HA        ▲ ᯤ 🔋│ barra 48 px   │ 14:32  ●HA                        ▲ ᯤ 🔋 │ 40 px
├──────────────────────────┤              ├──────────────────────────────────────────┤
│  ▢    ▢    ▢    ▢        │              │  ▢   ▢   ▢   ▢   ▢   ▢   ▢               │
│  ▢    ▢    ▢    ▢        │ grilla 4×6   │  ▢   ▢   ▢   ▢   ▢   ▢   ▢      grilla   │
│  ▢    ▢    ▢    ▢        │ (24 por      │  ▢   ▢   ▢   ▢   ▢   ▢   ▢      7×3      │
│  [ widget 4×2 HA  ]      │  página)     │                · ● ·                    │
│  ▢    ▢    ▢    ▢        │              │ ┌──────────────────────────────────────┐ │
│          · ● · ·         │ puntitos     │ │   ▢    ▢    ▢    ▢    ▢    ▢        │ │ dock
│ ┌──────────────────────┐ │              │ └──────────────────────────────────────┘ │
│ │  ▢    ▢    ▢    ▢    │ │ dock (4)     │                  ─────                   │
│ └──────────────────────┘ │              └──────────────────────────────────────────┘
│          ─────           │ indicador de inicio
└──────────────────────────┘
```

### Barra de estado (siempre visible salvo en apps `FULLSCREEN`)

- Izquierda: **hora**, y el nombre o el ícono de la app en primer plano.
- Centro (opcional): actividad en curso a lo "Dynamic Island" chiquito:
  grabación, captura serie corriendo, música sonando, programador trabajando.
- Derecha: WiFi (señal), BLE, **Home Assistant** (conectado/caído), MQTT, SD,
  USB (modo), módulos activos (un chip de enchufe), batería (sólo si hay una
  conectada), notificaciones sin leer.
- Tocar la izquierda tira del Centro de notificaciones; la derecha, del Centro
  de control.

### Pantalla de inicio

- Páginas de íconos que se pasan de costado. Los puntitos indican la página.
- **Modelo de datos = lista ordenada**, no posiciones absolutas: la misma lista
  se reparte en 4×6 (vertical) o 7×3 (horizontal). Así girar no desordena nada.
  Se puede forzar un corte con `page`.
- **Dock** con 4 apps (6 en horizontal), igual en todas las páginas.
- **Íconos**: los AIC de AmoledOS (vectoriales en %, se dibujan a cualquier
  tamaño) sobre un **cuadrado redondeado** (squircle) en lugar del círculo del
  reloj. Nombre debajo. **Globitos** (badges) con número, que pone la app o el SO.
- **Carpetas**: se abren como en iOS, en un panel sobre el fondo difuminado,
  grilla 3×3 con páginas, nombre editable. El ícono de la carpeta muestra en
  miniatura las 4 primeras apps (en vez de los hexágonos del reloj).
- **Widgets** de 2×2, 4×2 y 4×4 celdas, mezclados con los íconos: reloj, clima,
  entidades de HA (un interruptor, un sensor con gráfico), consumo de Claude,
  estado de la captura serie, cotizaciones. Los aporta el firmware o una app
  (`aos_widget_register`).
- **Modo edición** (mantener apretado en un ícono o en un hueco): los íconos
  tiemblan, se arrastran entre páginas y al dock, soltar un ícono sobre otro
  crea una carpeta, la ✕ quita del inicio (y ofrece borrar la `.so` si es
  dinámica), "+" arriba para agregar widgets.
- **Menú contextual** (mantener apretado corto): Abrir, Info de la app
  (versión, tamaño, permisos de módulos), Editar inicio, acciones rápidas que
  declare la app ("Nueva captura", "Última placa").
- **Biblioteca de apps**: última página, todas las apps por categoría y en
  orden alfabético, incluidas las ocultas.
- **Búsqueda** (deslizar hacia abajo en el medio del inicio): apps, ajustes,
  **entidades de HA**, archivos de la SD.
- **Fondo de pantalla**: degradado o imagen JPEG de la SD (decodificado por
  hardware), elegido en Ajustes. Mila de fondo por defecto (gatito negro sobre fondo claro).

### Gestos del sistema

| Gesto | Acción |
|---|---|
| Deslizar desde el borde inferior hacia arriba | Ir al inicio |
| Idem y mantener | Selector de apps (recientes, con miniatura) |
| Deslizar desde el borde izquierdo hacia la derecha | Atrás (llama `back()` de la app) |
| Bajar desde arriba a la izquierda | Centro de notificaciones |
| Bajar desde arriba a la derecha | Centro de control |
| Bajar en el medio del inicio | Búsqueda |
| Mantener en el inicio | Modo edición |
| Dos dedos (en apps que lo pidan) | Pinch/zoom, como en AmoledOS |

Las apps con `NO_SWIPE` o `LONG_DRAG` (juegos, dibujo) sólo reciben los gestos de
borde con el indicador "pegado": primer deslizamiento lo muestra, el segundo
actúa (como iOS con los juegos a pantalla completa).

### Centro de control

Grilla de módulos, igual en espíritu al de iOS:
- Brillo y volumen (deslizadores grandes)
- WiFi, BLE, No molestar, **Orientación (vertical/horizontal)**, Protector de
  pantalla ya, Captura de pantalla
- Música en curso (título, controles, tapa)
- Escenas de HA fijadas por el usuario (4-8 botones)
- Estado de módulos (RS485 activo, captura serie grabando) con acceso directo
- Temporizador / cronómetro corriendo

### Notificaciones

- Fuentes: el sistema (OTA, SD, módulo detectado, placa programada), las apps
  (`aos_hal_notif_post`), **Home Assistant** (un servicio `notify` que apunta a la
  P4 por MQTT o HTTP), **disparadores de la captura serie** ("apareció
  `Guru Meditation` en el puerto B").
- Banner que baja desde arriba unos segundos, globito en el ícono, historial en
  el Centro de notificaciones, agrupado por app.
- Reutiliza la política de AmoledOS (`aos_notif.c`: prioridades, No molestar
  programado, categorías).

### Multitarea

- AmoledOS tenía una app viva a la vez. Con 32 MB de PSRAM, P4OS mantiene
  **hasta N apps vivas** (4 para arrancar; se mide en la Fase 3): la que sale
  recibe `hide()` y deja de recibir `tick`, pero no se destruye. Al volver,
  `show()`. Si falta memoria se destruye la menos usada.
- Las apps con worker (juegos, cámaras) **pausan el worker en `hide()`**.
- **Servicios** (siguen corriendo sin app en pantalla): música/radio, conexión a
  HA, captura serie, programador, alarmas, módulos. Viven en firmware;
  a futuro, una `.so` también podrá registrar un servicio.
- El selector de apps muestra miniaturas (captura de LVGL reducida con el PPA).

### Protector y reposo

Es un aparato de escritorio siempre enchufado, así que no hay deep sleep:
- Tras X minutos sin tocar: **protector de pantalla**. Las esferas de AmoledOS
  (analógica, flip, nixie, anillos…) se portan acá, en grande, con widgets
  opcionales (HA, clima).
- Tras Y minutos más, o en el horario nocturno: retroiluminación al mínimo o apagada.
- Se despierta tocando la pantalla, o por una notificación importante (HA puede despertarla).

## Apps de sistema (en firmware)

Inicio, Ajustes, Archivos, Biblioteca, Centro de control, Notificaciones, y el
**Gestor de apps** (instalar `.so` desde la SD o el portal, versiones,
actualizaciones desde un catálogo). El resto está en [APPS.md](APPS.md).

## Estilo visual

- Modo oscuro por defecto (pantalla IPS: el negro no apaga píxeles como en el
  AMOLED, pero de noche molesta menos), modo claro opcional, color de acento.
- Paneles con esquinas redondeadas y fondo translúcido. El desenfoque real es
  caro a 720×1280: se **falsea** reduciendo la captura del fondo con el PPA,
  suavizándola y reescalándola, **una sola vez** al abrir la carpeta o el centro.
- Animaciones cortas (150-250 ms) y sólo sobre lo que cambia: en el reloj
  aprendimos que el costo es el área empujada, no el dibujo.

## Qué se reutiliza de AmoledOS y qué es nuevo

| Pieza | Destino |
|---|---|
| `aos_menu.c` (`menu.txt`) | Se extiende: `page`, `dock`, `widget`; las carpetas pasan de hexágono a panel |
| Íconos AIC | Tal cual; cambia sólo la forma de fondo |
| Centro de control, gestos, capas | Se reescriben con otra forma, se reusa la lógica |
| i18n (es/en/de, packs en SD) | Tal cual |
| Esferas | Pasan a ser protectores de pantalla |
| Superposición de notificaciones | Se reescribe como banner + centro |
| Lanzador (lista, grilla, panal) | Reemplazado por el inicio |
