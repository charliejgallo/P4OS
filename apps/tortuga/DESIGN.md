# Tortuga: el diseño del juego

G12 de `docs/internal/PEDIDOS-APPS-2026-11.md`, un juego que enseña a
programar en la línea de LOGO. Etapa 1, la lógica del juego, corregida con
el usuario el 2026-11-23 (sus decisiones están en la sección 10). Después
vienen la muestra del arte (etapa 2) y el juego completo (etapa 3).

Lo que ya anda de esto: el modelo de referencia en Python
(`tools/tg_ref.py`) con el mundo, el Logo propio, el contador de piezas y
los corredores de Lua y Python, y los quince niveles de ejemplo de la
sección 7 (`levels/muestra.tl`). Cada solución de ejemplo **se corrió** en
los tres lenguajes de texto, en todos los mapas de su nivel y en los dos
modos (fácil y difícil), y gana:

```
python3 apps/tortuga/tools/tg_ref.py check --lua-lib <liblua.dylib> apps/tortuga/levels/muestra.tl
```

(la `liblua.dylib` sale de la copia de Lua de la app Lua: la línea está en
la cabecera de `tg_ref.py`). El juego en C va a seguir ese modelo y va a
correr los mismos niveles contra él.

---

## 0. En una página

- **La app se llama Tortuga** (`apps/tortuga`, `demo.tortuga`).
- **Un juego de mundos y niveles.** En cada nivel hay un mapa, la tortuga y
  la meta; el jugador arma el programa, lo corre y mira a la tortuga
  hacerlo, paso a paso si quiere, con la línea que se está ejecutando
  marcada. Si choca, se cae al agua o no llega, se le explica en criollo
  qué pasó y en qué línea.
- **Cuatro formas de programar, el mismo juego:** **Bloques** (se tocan, sin
  teclado), **Logo** (un LOGO propio, en castellano o en inglés), **Lua**,
  y **Python** como instalación optativa. Los niveles no se escriben por
  lenguaje: son datos (el mapa, la meta, las órdenes que se permiten, las
  estrellas), y cada lenguaje es una forma de escribir las mismas órdenes.
  Todo nivel se juega en los cuatro.
- **Bloques y Logo son lo mismo por dentro**: un programa de bloques se
  puede ver escrito en Logo, en Lua o en Python con un toque ("así se
  escribe esto en Python"). Es el puente de los bloques al texto.
- **Ocho mundos, uno por idea:** secuencia → repetir → procedimientos →
  preguntas con sensores → mientras → variables → funciones con parámetros
  → el lápiz (dibujo como en LOGO). Más un mundo de desafíos mezclados y un
  modo libre para dibujar.
- **Dos modos:** en el fácil se gana al pasar por la meta; en el difícil la
  tortuga tiene que terminar el programa parada justo en la meta.
- **Estrellas por piezas:** ★ si llega, ★★ y ★★★ si lo hace con pocas
  piezas (órdenes y estructuras). Premia usar repetir y procedimientos. Se
  cuentan igual en los cuatro lenguajes. Con la tercera pista, ★★ como
  mucho.
- **Desde el mundo 4, cada nivel se prueba en varios mapas a la vez**: un
  programa escrito de memoria para un camino no sirve en el otro, hace
  falta preguntar. Es lo que hace que los sensores tengan sentido.

---

## 1. Los lenguajes

### 1.1 Lo que cuesta cada uno

| | Bloques | Logo (propio) | Lua | Python |
|---|---|---|---|---|
| Para quién | chicos desde ~7 años, sin teclado | desde ~9, el primer texto | quien quiere un lenguaje "de verdad" chico | el de la escuela y de afuera |
| Qué es | piezas que se tocan y se encastran | `avanzar 3`, `girar derecha`, `repetir 4 [ ... ]`, `para ... fin`, en castellano o inglés | Lua 5.4.8, el mismo de la app Lua | MicroPython 1.29, el mismo de la app Python |
| Cómo viene | con la app | con la app | con la app | **optativo**: se instala aparte |
| Peso en la tarjeta | 0 (usa el intérprete de Logo) | ~25 KB dentro de `tortuga.so` | ~130-150 KB dentro de `tortuga.so` | ~400 KB, un módulo aparte (`tg_python.so`) |
| Memoria mientras corre | casi nada | casi nada | ~100 KB de PSRAM | 256 KB a 1 MB de PSRAM (su heap) |
| Trabajo | **el más caro de la interfaz**: el editor de bloques | un intérprete chico (~1500 líneas de C), todo nuestro | copiar y atar (la app Lua ya resolvió casi todo) | el más caro de integrar: compilarlo como módulo con su configuración, y sacar la línea que corre |
| Errores | casi no hay: los bloques no se pueden escribir mal | 100% nuestros, en criollo | los de Lua, traducidos los comunes | los de Python, traducidos los comunes |
| Paso a paso | por pieza, con las variables a la vista | por línea, con las variables a la vista | por orden de la tortuga, con su línea | por orden de la tortuga, con su línea |

**Bloques.** Lo mejor en 5" y para chicos: no hay teclado ni errores de
tipeo. Por dentro son el árbol del programa de Logo, así que no suman
intérprete: el costo es la interfaz (sección 6.2).

**Logo, el propio.** El clásico: `avanzar 3`, `girar derecha`,
`repetir 4 [ avanzar 2 girar derecha ]`, `para escalon n ... fin`, con
las variables como en Lua y Python (`cuenta = 0`). Con las palabras en
castellano **y** en inglés (`forward 3`, `turn right`, `repeat 4 [ ... ]`,
`to step n ... end`); las del LOGO de siempre (`fd`, `rt 90`, `derecha`)
también andan, pero la paleta muestra `girar`. Al ser nuestro, cada error
se explica como queramos, el paso a paso va por línea y se pueden mostrar
las variables. Es el que mejor se escribe en la pantalla: casi sin signos
(sólo corchetes), sin paréntesis obligatorios, sin mayúsculas que importen.
La sintaxis entera está en la sección 2.3.

**Lua.** Ya está en el repo (`apps/lua/main/lua`, Lua 5.4.8 sin `io`, `os`
ni `debug`). Se copia a `apps/tortuga/main/lua/` y se compila en una sola
unidad con todo `static` (como el `onelua.c` que trae Lua), así no choca
con la app Lua cuando el simulador enlaza todas las apps juntas. La línea
que corre sale de `lua_getinfo` en cada orden de la tortuga, y un programa
que no termina se corta con el gancho de cuenta (`lua_sethook`, lo mismo
que hace `apps/lua/main/lua_app.c`). Lo que cuesta es poco.

**Python, optativo.** Es el que más se pide en la escuela, y el que más
cuesta:
- Un subconjunto propio ("algo que parece Python") enseñaría algo que no es
  Python: un `print` con `f"..."` o un `import` que ande en la computadora
  y acá no. Descartado.
- Es MicroPython de verdad, recortado (la tortuga no necesita red, consola
  ni la tarjeta): ~350-400 KB. Va como **módulo** que la app carga sólo
  cuando alguien elige Python (`aos_hal_module_open`, como los núcleos del
  Emulador). No puede llamarse `python.so`: el cargador identifica los
  módulos por el nombre del archivo y ese ya es de la app Python. En el
  simulador va enlazado, como los núcleos del Emulador.
- **Se instala aparte, si se quiere.** Sin él, la tarjeta de Python en
  "¿Cómo querés programar?" dice que hay que bajarlo de la Tienda. Cómo
  viaja un módulo optativo (hoy la Tienda tiene packs optativos, no
  módulos) es un pedido al firmware (sección 8).
- Lo que hay que medir: sacar la línea del programa en cada orden de la
  tortuga. MicroPython no lo da gratis; hay que encender
  `MICROPY_PY_SYS_SETTRACE` o leer el estado del código que corre. Va
  después de que el juego ande con los otros tres.

### 1.2 Cómo se elige

Al abrir el juego por primera vez: **"¿Cómo querés programar?"** con cuatro
tarjetas (Bloques, Logo, Lua, Python), cada una con un ejemplo de cuatro
líneas del mismo programa, y abajo el modo (fácil o difícil). Se cambia
cuando se quiera desde el menú del juego, y también **dentro de un nivel**
(el botón "Ver en..." muestra el programa de bloques escrito en el lenguaje
que se elija).

- **Todos los niveles se juegan en todos los lenguajes**, no sólo el
  tutorial: los niveles son datos. Las explicaciones y las pistas se
  escriben una vez, con las órdenes entre llaves (`{avanzar}`,
  `{repetir}`), y el juego las muestra en el lenguaje elegido.
- **Las estrellas se guardan por lenguaje.** Los mundos se abren con las
  estrellas de cualquiera de ellos. Así tiene sentido volver a pasar el
  mundo 2 en Lua después de pasarlo con bloques, y el mapa del mundo
  muestra, en cada nivel, de qué lenguajes ya tiene estrellas.
- **Logo en castellano o en inglés** sigue el idioma de la placa; el
  intérprete entiende las dos siempre (un programa puede mezclarlas). Lo
  mismo en Lua y Python: `avanzar()` y `forward()` andan las dos, y la
  paleta muestra las del idioma de la placa.

### 1.3 Los bloques y el texto

```
  Bloques                         Ver en Logo           Ver en Python
 ┌──────────────────────────┐
 │ ⟳ repetir  [ 4 ]         │    repetir 4 [           for _ in range(4):
 │ ┃ ⬆ avanzar              │      avanzar                 avanzar()
 │ ┃ ↶ girar izquierda      │      girar izquierda         girar_izquierda()
 │ ┃ ⬆ avanzar              │      avanzar                 avanzar()
 │ ┃ ↷ girar derecha        │      girar derecha           girar_derecha()
 │ ┗━━━━━━━━━━━━━━━━━━━━━━  │    ]
 └──────────────────────────┘
```

De bloques a cualquier texto se genera siempre. De texto a bloques, sólo
desde Logo (es nuestro el analizador); un programa de Lua o Python se
queda en texto.

---

## 2. Las órdenes de la tortuga (lo común a todos)

### 2.1 Las órdenes y los sensores

| Logo | Logo en inglés | Lua y Python | Qué hace |
|---|---|---|---|
| `avanzar n` | `forward n` | `avanzar(n)`, `forward(n)` | avanza n casillas (en el mapa) o n pasos (en la hoja); sin número, 1 |
| `retroceder n` | `back n` | `retroceder(n)`, `back(n)` | lo mismo, para atrás |
| `girar derecha g` | `turn right g` | `girar_derecha(g)`, `turn_right(g)` | gira g grados a la derecha; sin número, 90 |
| `girar izquierda g` | `turn left g` | `girar_izquierda(g)`, `turn_left(g)` | lo mismo, a la izquierda |
| `comer` | `eat` | `comer()`, `eat()` | come la lechuga de la casilla (si no hay, mastica pasto: no pasa nada) |
| `decir x` | `say x` | `decir(x)`, `say(x)` | la tortuga lo dice en un globito (un número, una palabra) |
| `bajar_lapiz` | `pen_down` | `bajar_lapiz()`, `pen_down()` | desde acá deja trazo (en la hoja arranca bajado) |
| `subir_lapiz` | `pen_up` | `subir_lapiz()`, `pen_up()` | deja de dibujar |
| `color c` | `color c` | `color(c)` | el color del trazo: `rojo`/`red`, ... o un número |

En Lua y Python, los espacios de una orden de Logo son guiones bajos
(`girar derecha` → `girar_derecha()`), igual que en `bajar_lapiz`. Logo
acepta además las del LOGO de siempre: `fd`, `bk`, `rt`, `lt`, `pd`, `pu`,
`derecha`, `izquierda`, `av`, `de`, `iz`.

Los **sensores** son preguntas de sí o no, y van en un `si` o un
`mientras` (en Lua y Python, con paréntesis: `camino_adelante()`):

| Castellano | Inglés | Es verdad si... |
|---|---|---|
| `camino_adelante` | `path_ahead` | se puede pisar la casilla de adelante |
| `camino_derecha` | `path_right` | se puede pisar la de la derecha |
| `camino_izquierda` | `path_left` | se puede pisar la de la izquierda |
| `agua_adelante` | `water_ahead` | adelante hay agua |
| `hay_lechuga` | `on_lettuce` | hay una lechuga donde está parada |
| `en_meta` | `at_goal` | está en la meta |
| `tengo_llave` | `has_key` | lleva una llave |

Sin acentos en los nombres (`bajar_lapiz`), así se escriben igual en Lua y
en Python, que no aceptan letras con acento en un nombre.

### 2.2 Lo mismo en cada lenguaje

| | Logo | Lua | Python |
|---|---|---|---|
| una orden | `avanzar 3` | `avanzar(3)` | `avanzar(3)` |
| girar | `girar derecha` | `girar_derecha()` | `girar_derecha()` |
| repetir | `repetir 4 [ ... ]` | `for i = 1, 4 do ... end` | `for _ in range(4):` |
| procedimiento | `para rodear ... fin` | `function rodear() ... end` | `def rodear():` |
| con parámetros | `para escalon n ... fin` y `escalon 3` | `function escalon(n)` y `escalon(3)` | `def escalon(n):` y `escalon(3)` |
| pregunta | `si hay_lechuga [ comer ]` | `if hay_lechuga() then comer() end` | `if hay_lechuga():` |
| si no | `si camino_adelante [ avanzar ] sino [ girar derecha ]` | `if ... then ... else ... end` | `if ...:` / `else:` |
| mientras | `mientras no en_meta [ ... ]` | `while not en_meta() do ... end` | `while not en_meta():` |
| variable | `cuenta = 0` y `cuenta = cuenta + 1` (o `cuenta += 1`) | `local cuenta = 0` y `cuenta = cuenta + 1` | `cuenta = 0` y `cuenta += 1` |
| comparar | `cuenta == 3` (o `cuenta = 3` adentro de una pregunta) | `cuenta == 3` | `cuenta == 3` |
| devolver un valor | `devolver x` | `return x` | `return x` |
| decir | `decir cuenta` | `decir(cuenta)` | `decir(cuenta)` |

### 2.3 Logo, entero

```
programa   := sentencia*
sentencia  := para NOMBRE param* sentencia* fin     ; los parámetros, en la misma línea
            | repetir expr [ sentencia* ]
            | si expr [ sentencia* ] (sino [ sentencia* ] | sino si ...)?
            | mientras expr [ sentencia* ]
            | NOMBRE = expr  |  NOMBRE += expr  |  NOMBRE -= expr
            | alto | devolver expr
            | girar (derecha|izquierda) expr?
            | ORDEN arg* | PROCEDIMIENTO arg*
expr       := o, y, no, == = <> < > <= >=, + -, * /, -x, (expr),
              números, "palabras", verdadero, falso, variables, sensores,
              procedimientos que devuelven algo
```

- Palabras clave en castellano y en inglés, siempre las dos: `para`/`to`,
  `fin`/`end`, `repetir`/`repeat`, `si`/`if`, `sino`/`else`,
  `mientras`/`while`, `girar`/`turn`, `derecha`/`right`,
  `izquierda`/`left`, `no`/`not`, `y`/`and`, `o`/`or`, `alto`/`stop`,
  `devolver`/`output`, `verdadero`/`true`, `falso`/`false`.
- Sin distinción de mayúsculas. `; comentario` hasta el fin de la línea.
- Las líneas no importan (se puede escribir todo en una), salvo los
  parámetros de `para`, que van en su misma línea.
- **Variables como en Lua y Python:** `cuenta = 0`, `cuenta = cuenta + 1`,
  `cuenta += 1`. Al principio de una sentencia, `=` es asignar; adentro
  de una pregunta, comparar (también vale `==`, que es lo que se usa en
  Lua y Python). El `haz cuenta 0` del LOGO de siempre también anda, y
  `:lado` se lee como `lado`.
- Los parámetros son locales del procedimiento; asignar un parámetro cambia
  el local, si no, la global.
- Recursión permitida, hasta 100 llamadas adentro (para los árboles y los
  fractales del mundo del lápiz).
- Una orden con número optativo (`avanzar`, `girar derecha`) toma lo que
  sigue si es un valor: `avanzar n girar izquierda` es `avanzar(n)` y
  después `girar_izquierda()`.

---

## 3. Los niveles como datos

### 3.1 El formato

Un archivo de texto (`.tl`) con varios niveles. Cada línea que empieza con
`@` abre un campo; lo que sigue hasta la próxima `@` es su contenido. `@--`
es un comentario.

```
@nivel 4-1
@titulo es: Lechugas salteadas
@titulo en: Scattered lettuce
@ordenes avanzar girar_derecha girar_izquierda comer hay_lechuga
@meta lechugas llegar
@estrellas 4 6
@mapa
##########
#>.L..L.M#
##########
@mapa
##########
#>L.L..LM#
##########
@pista es: Tu programa se prueba en tres caminos. En cada casilla preguntá si hay lechuga.
@logo
repetir 7 [
  avanzar
  si hay_lechuga [ comer ]
]
@lua
...
@python
...
```

| Campo | Qué dice |
|---|---|
| `@nivel` | mundo-nivel |
| `@titulo es:` / `en:` / `de:` | el nombre, por idioma |
| `@ordenes` | las órdenes y sensores que se pueden usar (la paleta muestra sólo esas; en texto, usar otra es un error explicado). Las estructuras (`repetir`, `si`...) se abren por mundo, no por nivel |
| `@meta` | lo que hay que lograr, todo junto: `llegar` (la meta), `lechugas` (comerlas todas), `decir=lechugas` (decir cuántas hay), `dibujo` (dibujar la figura) |
| `@estrellas a b` | ★★★ con a piezas o menos, ★★ con b o menos |
| `@mapa` | un mapa; si hay varios, **el programa tiene que ganar en todos** |
| `@inicio x y dir` | dónde arranca la tortuga si no lo dice el mapa (o en la hoja, en pasos) |
| `@figura` | en los niveles de dibujo, el programa de Logo que dibuja la figura pedida |
| `@pista es:` | una pista; varias, de la más suave a la más directa |
| `@logo`, `@lua`, `@python` | la solución de referencia: la usa el verificador y la última pista |

### 3.2 El mapa

| Letra | Casilla |
|---|---|
| `.` | pasto |
| `#` | piedra: no se pasa |
| `~` | agua: no se pasa (la tortuga se cae) |
| `=` | puente sobre el agua |
| `L` | lechuga, sobre pasto |
| `k` | llave: se junta sola al pasar |
| `P` | puerta: se abre sola si se lleva una llave (y la gasta) |
| `M` | la meta |
| `>` `<` `^` `v` | la tortuga, mirando para ese lado, sobre pasto |
| (afuera del mapa) | el borde: no se pasa |

**Las reglas del mundo:**
- La tortuga mira a uno de cuatro lados y gira de a 90 grados.
- Chocar con una piedra, el borde o una puerta cerrada, o meterse al agua,
  **termina el programa** con la explicación (y la animación de choque o de
  chapuzón). No hay vidas: se corrige y se corre de nuevo.
- `comer` donde no hay lechuga no es un error: la tortuga mastica pasto.
- Con varios mapas, el programa se corre en cada uno; se ve el primero
  entero y los otros rápido, con una marca por mapa. Si falla en uno, se
  muestra ese.
- Un programa que no termina se corta a las 5000 órdenes de la tortuga o a
  los 200 000 pasos del intérprete: "la tortuga se quedó dando vueltas: ¿el
  mientras tiene cómo terminar?".

**Cuándo se gana: el modo.** Se elige al empezar y se cambia en el menú.

| | Fácil | Difícil |
|---|---|---|
| La meta | se gana **al pasar** por ella con todo lo demás cumplido (las lechugas, lo que había que decir), aunque al programa le queden órdenes | se gana sólo si el programa **termina con la tortuga parada justo en la meta** y todo lo demás cumplido |
| Si se pasa de largo | ganó | "la tortuga pasó por la meta pero no se quedó ahí" |
| Si termina antes | "la tortuga se quedó a 3 casillas de la meta" | lo mismo |
| Las estrellas | iguales | iguales, y el nivel queda marcado con una medalla de difícil |

Los quince niveles de ejemplo tienen soluciones que ganan en los dos modos
(el verificador corre los dos). Las estrellas no cambian con el modo: lo
que cambia es lo justo que tiene que ser el programa.

### 3.3 La hoja (los niveles de dibujo)

- Una hoja de 400 × 400 pasos (de -200 a 200), la tortuga arranca donde
  diga `@inicio` mirando para arriba, con el lápiz bajado.
- `avanzar` va en pasos y `girar` en grados, cualquier ángulo: LOGO de
  verdad. Salirse de la hoja es un error.
- **La figura se escribe en Logo** (`@figura`). El juego la corre por su
  cuenta, la muestra punteada de fondo, y al final compara: cada punto de
  la figura tiene que estar a menos de 3 pasos de lo dibujado, y cada punto
  de lo dibujado a menos de 3 de la figura. El orden, el sentido y pasar
  dos veces por el mismo lado no importan; el color tampoco, salvo que el
  nivel lo pida. En la hoja el modo no cambia nada.

### 3.4 Dónde viven los niveles

- **Los del juego van adentro de `tortuga.so`:** un script de la app
  (`tools/gen_levels.py`) arma un `.c` con los `.tl` de `levels/`. Son
  pocos KB.
- **Mundos extra, y historias (la puerta abierta a packs):** la app lee,
  además de los suyos, todo `.tl` de `apps/tortuga/` en la tarjeta y el
  texto de cualquier pack `apps/tortuga_<algo>_p4.pak` (entradas `PK_TEXT`
  que se llamen `levels...`). Así la Tienda puede sumar mundos o historias
  como packs optativos sin una app nueva, y una maestra puede escribir los
  suyos (más adelante, con un editor de niveles en la página del portal).
  Un pack usa los mundos del 10 en adelante y sus propios ids, y los nombra:

  ```
  @mundo 10
  @titulo es: La isla del tesoro
  @idea es: Una historia en cinco capítulos
  @nivel 10-1
  @carta es: Capítulo 1. La tortuga llegó a la isla en un barquito.
  @carta es: Tiene que encontrar el cofre. ¿La ayudás?
  ...
  ```

  `@carta` (hasta cuatro por nivel e idioma) son tarjetas que se muestran
  antes del nivel mientras no tenga estrellas en ese lenguaje: el juego
  las usa para presentar la idea de cada mundo, y una historia, para
  contar su capítulo. Los mundos de un pack están siempre abiertos.
- **El verificador:** `tools/tg_ref.py check` corre cada solución de
  referencia en cada mapa, en cada lenguaje y en los dos modos, y exige que
  las piezas den igual en todos y coincidan con las ★★★. Lo mismo corre el
  simulador (`TG_SELFTEST=1`) contra el intérprete en C: si el C y el
  modelo no dan igual, uno de los dos está mal.

---

## 4. Piezas, estrellas y pistas

**Una pieza es una sentencia:** una orden, la llamada a un procedimiento,
`repetir`, `si`, `mientras`, una asignación (`cuenta = 0`), `para`/`def`/
`function`, `alto`/`return`. No cuentan `sino`/`else`, los números ni lo
que va adentro de una condición. En bloques, una pieza es un bloque, que es
lo mismo.

En Lua y Python se cuentan sobre las palabras del programa con la misma
regla (`for`, `while`, `if`, `elif`/`elseif`, `def`/`function`, `return`,
una asignación, y una llamada que empieza una sentencia). Los quince
niveles de ejemplo dan la misma cuenta en los tres lenguajes de texto.

| | |
|---|---|
| ★ | llegó (cumplió `@meta` en todos los mapas) |
| ★★ | con `b` piezas o menos: más o menos lo que sale sin la idea nueva del mundo, bien hecho |
| ★★★ | con `a` piezas o menos: lo que sale usando la idea del mundo (la solución de referencia) |

Además se muestran, sin estrellas, los **pasos** que dio la tortuga.

**Pistas**, tres por nivel, que se abren de a una:
1. Un empujón en palabras ("¿viste que el camino se repite?").
2. La tarjeta del concepto del mundo, con un ejemplo de otro problema.
3. El esqueleto de la solución, con huecos.

Las dos primeras no cuestan nada; **la tercera deja ese nivel en ★★ como
mucho** (se puede volver a jugar sin pistas para la tercera).

---

## 5. La progresión

### 5.1 Los mundos

| Mundo | Idea | Lo nuevo | Niveles | Ejemplo del último nivel |
|---|---|---|---|---|
| 1. Primeros pasos | secuencia | `avanzar`, `girar derecha`, `girar izquierda`, `comer`; llaves y puertas | 6 | juntar la llave, abrir la puerta y llegar |
| 2. Otra vez | repetir | `repetir` | 6 | una escalera y la vuelta a la laguna |
| 3. Ponele nombre | procedimientos | `para ... fin` | 5 | dos piedras que se rodean igual |
| 4. Preguntar | si / sino, sensores | `si`, `sino`, `hay_lechuga`, `agua_adelante`, `camino_*`; **varios mapas** | 6 | charcos en lugares distintos en cada mapa |
| 5. Hasta que | mientras | `mientras`, `no`, `en_meta` | 6 | el laberinto de la mano derecha |
| 6. Contar | variables | `cuenta = 0`, `decir`, `+=`, `-=` | 4 | contar lechugas y decir cuántas hay |
| 7. A medida | parámetros | `para fila n` | 3 | escalones de todos los tamaños |
| 8. El lápiz | dibujo LOGO | la hoja, `girar derecha 144`, `bajar_lapiz`, `color` | 8 | cuadrado, triángulo, casa, estrella, polígono de n lados, espiral, flor, árbol |
| 9. La gran travesía | todo junto | nada nuevo | 3 | el laberinto con lechugas, charcos sin contar, un molinete de cuadrados |

**47 niveles**, en `levels/mundos.tl` (todos verificados con
`tools/check_levels.sh`). Cada mundo arranca con tarjetas (`@carta`) que
presentan la idea, y sigue con un nivel donde la idea es casi obligatoria,
los de práctica y un **desafío** al final.

**Modo libre:** la hoja en blanco, sin meta, con todas las órdenes, para
dibujar lo que se quiera; el dibujo se guarda como imagen en la tarjeta. Y
"Mis programas": lo escrito en cada nivel queda guardado por lenguaje.

### 5.2 Cómo sube la dificultad

Se mide por nivel, y el verificador puede listarlo:
- **piezas** de la solución de referencia (de 3 a ~15);
- **anidamiento** (un `si` dentro de un `repetir` dentro de un `para`);
- **cantidad de mapas** (uno, o varios que obligan a preguntar);
- **pasos** que da la tortuga (cuánto hay que imaginarse sin verlo);
- **el modo:** el difícil pide además que el programa termine justo en la
  meta, sin pasarse.

Dentro de un mundo, sube de a una de esas cosas por nivel. Una idea nueva
llega siempre en un nivel chico (3-5 piezas), y la idea anterior se sigue
usando en el mundo siguiente (el mundo 5 usa `si`, el 6 usa `mientras`).

---

## 6. Escribir y correr en 5"

### 6.1 La pantalla

De pie (720 × 1280): el mapa arriba, el programa abajo.

```
┌──────────────────────────────┐
│ ◀  4-1 Lechugas salteadas ★☆☆│  título, estrellas, mapas: ● ○ ○
├──────────────────────────────┤
│                              │
│        (el mapa, ~560 px)    │
│     🐢 . L . . L . 🏁        │
│                              │
├──────────────────────────────┤
│ ▶ Correr  ⏭ Paso  ⏹  🐢🐇   │  correr, paso a paso, parar, velocidad
├──────────────────────────────┤
│  1  repetir 7 [              │
│  2    avanzar                │  ← la línea que corre, marcada
│  3    si hay_lechuga [ comer]│
│  4  ]                        │
├──────────────────────────────┤
│ avanzar  girar der.  girar iz│  paleta: sólo las órdenes del nivel
│ comer  repetir  si  hay_lech…│
├──────────────────────────────┤
│        (el teclado)          │  se esconde con un teclado físico
└──────────────────────────────┘
```

Acostada (1280 × 720): el mapa a la izquierda (720 × 720), el programa y la
paleta a la derecha.

### 6.2 Bloques

- **Una lista, no un lienzo libre.** El programa es una columna de bloques,
  con los de adentro de un `repetir` o un `si` corridos a la derecha. En 5"
  un lienzo donde se arrastra todo a cualquier lado es incómodo; una lista
  se toca bien con el dedo.
- **Una barra de inserción** marca dónde va lo próximo. Tocar un bloque de
  la paleta lo pone ahí. Tocar entre dos bloques mueve la barra.
- Un bloque que tiene adentro (`repetir`, `si`, `para`) deja la barra
  adentro suyo al ponerlo.
- **Tocar un número** abre una rueda chica (− 3 +). Mantener apretado un
  bloque lo levanta para moverlo; deslizarlo a la izquierda lo borra.
- Colores por tipo: movimiento verde, repetir naranja, preguntas
  violeta, procedimientos azul, variables amarillo, lápiz rosa.

### 6.3 Texto (Logo, Lua, Python)

- **Un editor propio** con números de línea, fuente mono (`aos_mono_22`),
  la línea que corre marcada y la del error en rojo. No un `lv_textarea`:
  hace falta marcar líneas y pintar las palabras.
- **La paleta** pone la orden con todo lo que lleva (`avanzar(1)` con el 1
  elegido, `girar derecha` de un toque, `repetir 4 [` con su `]` en la línea
  de abajo, `for _ in
  range(4):` con la sangría). Es la forma principal de escribir en la
  pantalla.
- **Autocompletar:** una barra arriba del teclado con lo que puede seguir
  (las órdenes del nivel, las variables y los procedimientos del
  programa), como la de la consola de Python.
- **Sangría sola** después de `[`, `do`, `then`, `:`; los cierres (`]`,
  `end`, `fin`) se ponen solos al abrir.
- **Un teclado propio de programar** en la pantalla: letras, una fila con
  `( ) [ ] = + - : _` y los números siempre a la vista.
- **Teclado físico, USB o Bluetooth** (`aos_ui_hwkbd_handler`): esconde el
  de la pantalla. Ctrl+R corre, Ctrl+. para, Ctrl+Espacio autocompleta,
  Ctrl+Z deshace, Tab sangra.

### 6.4 Correr

- **El programa corre entero primero, y después se anima.** El intérprete
  corre en un hilo, con el mundo, y anota cada orden y cada pregunta con su
  línea (un rastro). La pantalla reproduce el rastro. Así:
  - se puede ir **un paso para atrás** y adelantar arrastrando una barra;
  - la animación nunca espera al intérprete;
  - un programa que no termina se corta antes de que se vea nada raro.
- **Velocidades:** tortuga (un paso por segundo), normal, liebre, y "al
  toque" (el final directo).
- **Paso a paso:** cada toque de ⏭ avanza una orden. En Bloques y Logo
  se ven las variables con su valor en ese momento.
- La tortuga con **globito** para `decir` y para los errores.

### 6.5 Los errores, en criollo

| Pasa | Dice |
|---|---|
| choca | "En la línea 5, la tortuga chocó con una piedra. Estaba mirando para el este." |
| agua | "En la línea 3 la tortuga se metió al agua. ¿Había un puente más allá?" |
| puerta | "La puerta está cerrada y la tortuga no tiene llave." |
| no llega | "El programa terminó y la tortuga quedó a 2 casillas de la meta." |
| se pasa (difícil) | "La tortuga pasó por la meta pero no se quedó ahí: en el modo difícil tiene que terminar justo encima." |
| falta algo | "Llegó, pero quedan 2 lechugas sin comer." |
| no termina | "La tortuga se quedó dando vueltas: ¿el mientras tiene cómo terminar?" |
| varios mapas | "En el primer camino anduvo, en el segundo no: probá preguntar en vez de contar." |
| orden de otro nivel | "En este nivel todavía no está «retroceder»." |
| girar sin lado | "Línea 3: después de «girar» va derecha o izquierda." |
| no se entiende (Logo) | "Línea 2: no conozco «avanzr». ¿Quisiste decir «avanzar»?" |
| falta un cierre | "Línea 4: falta cerrar el bloque con ]." |
| Lua | `attempt to call a nil value (global 'avanzr')` → el mismo "no conozco «avanzr»" |
| Python | `NameError: name 'avanzr' isn't defined` → lo mismo; `IndentationError` → "la línea 3 tiene que estar corrida igual que la de arriba" |

Los mensajes de Lua y de Python se traducen con una tabla de los más
comunes (nombre que no existe, falta un `end` o un `:`, sangría, tipos);
los que no estén en la tabla salen tal cual, con la línea.

### 6.6 Joystick

`aos_pad.h`: la cruceta mueve la barra de inserción por los bloques (o el
cursor por las líneas), A pone el bloque elegido de la paleta, L y R
cambian de bloque en la paleta, B borra, START corre o para, y en el modo
libre la cruceta **maneja la tortuga a mano y el juego escribe el
programa** mientras tanto. Es una buena primera clase para chicos: manejar
y ver aparecer el código.

---

## 7. Quince niveles de ejemplo

Todos en `levels/muestra.tl` y verificados con `tools/tg_ref.py` (los tres
lenguajes de texto ganan en todos los mapas y cuentan las mismas piezas).
Las soluciones ganan en el modo fácil y en el difícil. Los bloques son el
mismo árbol que Logo; están dibujados en algunos.

Leyenda de los mapas: `.` pasto, `#` piedra, `~` agua, `L` lechuga, `k`
llave, `P` puerta, `M` meta, `>` la tortuga mirando al este.

### 1-3. La esquina (secuencia) — ★★★ ≤5, ★★ ≤7

```
######
#>..##
###.##
###.M#
######
```

```
Logo               Lua                  Python
avanzar 2          avanzar(2)           avanzar(2)
girar derecha      girar_derecha()      girar_derecha()
avanzar 2          avanzar(2)           avanzar(2)
girar izquierda    girar_izquierda()    girar_izquierda()
avanzar            avanzar()            avanzar()
```

En inglés: `forward 2  turn right  forward 2  turn left  forward`.

### 1-5. La llave y la puerta — ★★★ ≤3, ★★ ≤4

```
########
#>.k...#
######P#
######.#
######M#
########
```

```
Logo               Lua                  Python
avanzar 5          avanzar(5)           avanzar(5)
girar derecha      girar_derecha()      girar_derecha()
avanzar 3          avanzar(3)           avanzar(3)
```

La llave se junta al pasar y la puerta se abre sola: sin órdenes nuevas.

### 2-2. La escalera (repetir) — ★★★ ≤5, ★★ ≤9

```
#######
#####M#
####..#
###..##
##..###
#>.####
#######
```

Bloques:

```
⟳ repetir [4]
┃ ⬆ avanzar
┃ ↶ girar izquierda
┃ ⬆ avanzar
┃ ↷ girar derecha
┗━━━━
```

```
-- Logo
repetir 4 [
  avanzar
  girar izquierda
  avanzar
  girar derecha
]
-- Lua
for i = 1, 4 do
  avanzar()
  girar_izquierda()
  avanzar()
  girar_derecha()
end
# Python
for _ in range(4):
    avanzar()
    girar_izquierda()
    avanzar()
    girar_derecha()
```

Sin `repetir` son 16 piezas: ★.

### 2-4. La vuelta a la laguna — ★★★ ≤4, ★★ ≤7

La meta está donde arranca la tortuga: hay que dar la vuelta entera
comiendo las tres lechugas.

```
######
#M..L#      la tortuga arranca en la M, mirando al este
#.~~.#
#.~~.#
#L..L#
######
```

```
-- Logo
repetir 4 [ avanzar 3 comer girar derecha ]
-- Lua
for i = 1, 4 do avanzar(3) comer() girar_derecha() end
# Python
for _ in range(4):
    avanzar(3)
    comer()
    girar_derecha()
```

En la cuarta vuelta la tortuga pisa la meta con todo comido y gana: el
último `comer` nunca corre.

### 3-2. Dos piedras (procedimientos) — ★★★ ≤13, ★★ ≤17

```
###########
##...#...##
#>.#...#.M#
###########
```

```
-- Logo
para rodear
  girar izquierda
  avanzar
  girar derecha
  avanzar 2
  girar derecha
  avanzar
  girar izquierda
fin
avanzar
rodear
avanzar 2
rodear
avanzar
-- Lua
function rodear()
  girar_izquierda() avanzar() girar_derecha() avanzar(2)
  girar_derecha() avanzar() girar_izquierda()
end
avanzar() rodear() avanzar(2) rodear() avanzar()
# Python
def rodear():
    girar_izquierda()
    avanzar()
    girar_derecha()
    avanzar(2)
    girar_derecha()
    avanzar()
    girar_izquierda()

avanzar()
rodear()
avanzar(2)
rodear()
avanzar()
```

Sin procedimiento son 17 (★★). La fila de arriba está cortada entre las
dos piedras: no hay atajo.

### 4-1. Lechugas salteadas (si, tres mapas) — ★★★ ≤4, ★★ ≤6

```
##########     ##########     ##########
#>.L..L.M#     #>L.L..LM#     #>...L..M#
##########     ##########     ##########
```

```
-- Logo
repetir 7 [
  avanzar
  si hay_lechuga [ comer ]
]
-- Lua
for i = 1, 7 do
  avanzar()
  if hay_lechuga() then comer() end
end
# Python
for _ in range(7):
    avanzar()
    if hay_lechuga():
        comer()
```

Un programa escrito para el primer mapa (`avanzar 2 comer avanzar 3
comer...`) gana ahí y en el segundo falla: "acá no hay ninguna lechuga".

### 4-3. Charcos (si / sino, con un procedimiento) — ★★★ ≤12, ★★ ≤16

```
###########     ###########     ###########
#...#...###     ###...#...#     #.........#
#>~...~..M#     #>..~...~M#     #>~.~.~.~M#
###########     ###########     ###########
```

Bloques:

```
▣ para rodear
┃ ↶ girar izquierda  ⬆ avanzar  ↷ girar derecha  ⬆ avanzar [2]
┃ ↷ girar derecha  ⬆ avanzar  ↶ girar izquierda
┗━━━━
⟳ repetir [4]
┃ ◇ si ‹agua adelante›
┃ ┃ ▣ rodear
┃ ┣ sino
┃ ┃ ⬆ avanzar [2]
┃ ┗━━━━
┗━━━━
```

```
-- Logo
para rodear
  girar izquierda avanzar girar derecha avanzar 2 girar derecha avanzar girar izquierda
fin
repetir 4 [
  si agua_adelante [ rodear ] sino [ avanzar 2 ]
]
-- Lua
function rodear()
  girar_izquierda() avanzar() girar_derecha() avanzar(2)
  girar_derecha() avanzar() girar_izquierda()
end
for i = 1, 4 do
  if agua_adelante() then rodear() else avanzar(2) end
end
# Python
def rodear():
    girar_izquierda()
    avanzar()
    girar_derecha()
    avanzar(2)
    girar_derecha()
    avanzar()
    girar_izquierda()

for _ in range(4):
    if agua_adelante():
        rodear()
    else:
        avanzar(2)
```

### 5-1. Hasta el fondo (mientras) — ★★★ ≤4, ★★ ≤5

```
#######     #########     ####
#>...##     #>......#     #>.#
####.##     #######.#     ##.#
####.##     #######M#     ##.#
####M##     #########     ##.#
#######                   ##M#
                          ####
```

```
-- Logo
mientras no en_meta [
  si camino_adelante [ avanzar ] sino [ girar derecha ]
]
-- Lua
while not en_meta() do
  if camino_adelante() then avanzar() else girar_derecha() end
end
# Python
while not en_meta():
    if camino_adelante():
        avanzar()
    else:
        girar_derecha()
```

La solución "obvia" (`mientras camino_adelante [avanzar] girar derecha mientras
camino_adelante [avanzar]`) tiene 5 piezas, ★★. La general, que no sabe
cuántos tramos hay, gana con 4.

### 5-4. El laberinto (la mano derecha) — ★★★ ≤7, ★★ ≤9

```
#########     #########
#>..#...#     #>......#
###.#.#.#     #.#####.#
#...#.#.#     #.#M..#.#
#.###.#.#     #.###.#.#
#.....#M#     #.....#.#
#########     #######.#
              #.......#
              #########
```

```
-- Logo
mientras no en_meta [
  si camino_derecha [
    girar derecha
    avanzar
  ] sino [
    si camino_adelante [ avanzar ] sino [ girar izquierda ]
  ]
]
-- Lua
while not en_meta() do
  if camino_derecha() then
    girar_derecha()
    avanzar()
  elseif camino_adelante() then
    avanzar()
  else
    girar_izquierda()
  end
end
# Python
while not en_meta():
    if camino_derecha():
        girar_derecha()
        avanzar()
    elif camino_adelante():
        avanzar()
    else:
        girar_izquierda()
```

`mientras no en_meta [ si camino_adelante [avanzar] sino [derecha] ]` (la
del 5-1) acá se queda dando vueltas, y el juego lo dice.

### 6-2. Contá las lechugas (variables) — ★★★ ≤6, ★★ ≤8

```
###########     ###########     ###########
#>.L..L.LM#     #>LL.....M#     #>.L.LL.LM#
###########     ###########     ###########
```

Hay que llegar y decir cuántas lechugas había (3, 2 y 4).

```
-- Logo
cuenta = 0
repetir 8 [
  avanzar
  si hay_lechuga [ cuenta = cuenta + 1 ]
]
decir cuenta
-- Lua
local cuenta = 0
for i = 1, 8 do
  avanzar()
  if hay_lechuga() then cuenta = cuenta + 1 end
end
decir(cuenta)
# Python
cuenta = 0
for _ in range(8):
    avanzar()
    if hay_lechuga():
        cuenta += 1
decir(cuenta)
```

`decir 3` gana en el primer mapa y en los otros dos no.

### 7-2. Escalones de todos los tamaños (parámetros) — ★★★ ≤8, ★★ ≤11

```
#########
#######M#
#######.#
#######.#
####....#
####.####
##...####
#>.######
#########
```

```
-- Logo
para escalon n
  avanzar n
  girar izquierda
  avanzar n
  girar derecha
fin
escalon 1
escalon 2
escalon 3
-- Lua
function escalon(n)
  avanzar(n) girar_izquierda() avanzar(n) girar_derecha()
end
escalon(1) escalon(2) escalon(3)
# Python
def escalon(n):
    avanzar(n)
    girar_izquierda()
    avanzar(n)
    girar_derecha()

escalon(1)
escalon(2)
escalon(3)
```

### 8-1. El cuadrado (el lápiz) — ★★★ ≤3, ★★ ≤9

Un cuadrado de 100 pasos de lado, desde (-50, -50), mirando para arriba.

```
-- Logo
repetir 4 [ avanzar 100 girar derecha 90 ]
-- Lua
for i = 1, 4 do avanzar(100) girar_derecha(90) end
# Python
for _ in range(4):
    avanzar(100)
    girar_derecha(90)
```

Sin `repetir` son 7 piezas (★★).

### 8-3. La estrella — ★★★ ≤3, ★★ ≤11

```
-- Logo                       -- Lua
repetir 5 [                      for i = 1, 5 do
  avanzar 150                      avanzar(150)
  girar derecha 144                      girar_derecha(144)
]                                end
# Python
for _ in range(5):
    avanzar(150)
    girar_derecha(144)
```

Con `girar derecha 72` sale un pentágono (y en este nivel, además, se sale de la
hoja): la pista 2 cuenta que la estrella da dos vueltas enteras, 720°.

### 8-6. La espiral (variables en la hoja) — ★★★ ≤5, ★★ ≤41

```
-- Logo
lado = 10
repetir 20 [
  avanzar lado
  girar derecha 90
  lado = lado + 10
]
-- Lua
local lado = 10
for i = 1, 20 do
  avanzar(lado)
  girar_derecha(90)
  lado = lado + 10
end
# Python
lado = 10
for _ in range(20):
    avanzar(lado)
    girar_derecha(90)
    lado += 10
```

### 8-8. La flor (funciones con parámetros, dibujando) — ★★★ ≤7, ★★ ≤10

Doce hexágonos girados de a 30°.

```
-- Logo
para poligono lados largo
  repetir lados [ avanzar largo girar derecha 360 / lados ]
fin
repetir 12 [ poligono 6 50 girar derecha 30 ]
-- Lua
function poligono(lados, largo)
  for i = 1, lados do avanzar(largo) girar_derecha(360 / lados) end
end
for i = 1, 12 do poligono(6, 50) girar_derecha(30) end
# Python
def poligono(lados, largo):
    for _ in range(lados):
        avanzar(largo)
        girar_derecha(360 / lados)

for _ in range(12):
    poligono(6, 50)
    girar_derecha(30)
```

---

## 8. Cómo se arma (para la etapa 3)

| Parte | Archivo | Qué hace |
|---|---|---|
| La app | `main/tg_app.c` | ciclo de vida, menús, guardado |
| El mundo | `main/tg_world.c` | el mapa, la hoja, las reglas, el rastro; lo mismo que `World` en `tg_ref.py` |
| Logo | `main/tg_logo.c` | analizador e intérprete; el árbol es también el de los bloques; genera Logo, Lua y Python desde el árbol |
| Lua | `main/tg_lua.c` + `main/lua/` | Lua 5.4.8 en una sola unidad, todo `static`; las órdenes como funciones, la línea con `lua_getinfo` |
| Python | `py/` → `tg_python.so` | MicroPython recortado como módulo optativo (después de los otros tres) |
| Correr | `main/tg_run.c` | el hilo que corre el programa con su presupuesto y llena el rastro (`aos_hal_thread_start`, que se espera en `destroy()`) |
| Bloques | `main/tg_blocks.c` | el editor de bloques |
| Texto | `main/tg_edit.c`, `main/tg_kb.c` | el editor y el teclado de programar |
| El dibujo | `main/tg_gfx.c` | el compositor de Bastión (`bs_gfx.c`) con prefijo `tg_`: mosaicos de 16 px que se redibujan sólo si cambiaron, sprites de la tortuga encima |
| Los niveles | `levels/*.tl` → `main/tg_levels.c` | generado por `tools/gen_levels.py` |
| El arte | `tools/blender/*.py` → `tortuga_p4.pak` | la tortuga y los mosaicos, renderizados (etapa 2) |

- **Prefijo `tg_`** en todo (no lo usa nadie en `apps/`).
- **Memoria:** el rastro, hasta 5000 órdenes de ~16 bytes, va en PSRAM; el
  mapa más grande, 16 × 16 casillas. Nada grande en RAM interna.
- **Guardado:** en `aos_hal_path_data()`: las estrellas por nivel y por
  lenguaje, y el último programa de cada nivel en cada lenguaje.
- **Python optativo:** el módulo `tg_python.so` en la carpeta de la app
  de la tarjeta; si no está, la tarjeta de Python lo dice. Cómo lo
  reparte la Tienda es un pedido al firmware (2026-11-23).
- **Página del portal** (más adelante, si suma): escribir programas y
  niveles desde la computadora, para una maestra.
- **Sonido:** `aos_hal_beep` para pasos y choques, y un festejo corto al
  ganar.

## 9. El arte

**La muestra (etapa 2, 2026-11-23).** Todo armado por código en Blender
3.3 (Cycles en Metal), renders livianos de 24 muestras, unos 40 segundos
en total:

```
Blender -b -P apps/tortuga/tools/blender/board.py  -- --out <dir>
Blender -b -P apps/tortuga/tools/blender/turtle.py -- --out <dir>
python3 apps/tortuga/tools/compose.py --art <dir> --out <dir>/muestra
```

- **La cámara** (`tools/blender/tg_common.py`, la base de Bastión con otras
  medidas): ortográfica y fija, mirando 48,6° hacia abajo. Una casilla es
  un metro: **64 px de ancho y 48 de fondo**, así entran 11 casillas en la
  pantalla de pie. Al ser ortográfica, un mosaico o un sprite sirve en
  cualquier lugar del tablero.
- **El tablero es de juguete** (`tools/blender/board.py`): cada casilla es
  un bloquecito, pasto arriba y tierra a los costados, y el agua está más
  baja, así que el frente del pasto se ve sobre el agua. El juego dibuja
  las filas de atrás para adelante, primero el piso, después lo que hay
  encima y la tortuga, y **cualquier mapa sale de las mismas piezas**: los
  niveles son datos, el arte no se dibuja por mapa. Las piezas son pasto
  (cuatro, dos con flores), agua (cuatro cuadros que ondean), piedras,
  lechuga, llave (flota), puerta (cerrada y abierta, de frente y de
  costado), la meta (un pie dorado con una estrella y una bandera a
  cuadros que flamea) y el puente (de los dos lados).
- **La tortuga** (`tools/blender/turtle.py`): caparazón color caramelo con
  las uniones de las placas en crema, piel verde clara, ojos grandes y una
  bufanda roja. Caminar en 16 direcciones (girar es pasar por ellas) con 6
  cuadros, y chocar, comer y festejar en 4 direcciones. Celda de 112 ×
  112 px.
- **La composición** (`tools/compose.py`): el mapa de `levels/arte.tl`
  armado como lo va a hacer el juego, con el programa fijo corrido por el
  modelo de referencia, y dos pantallas de ejemplo (de pie y acostada) con
  el diseño de la sección 6, dibujadas con Pillow.

**Lo que falta para el juego completo:** la hoja de dibujo, la caída al
agua, el globito de `decir`, mundos con otro piso (arena, nieve) y el
paquete `.pak`.

## 10. Lo que decidió el usuario (2026-11-23)

1. **El nombre: Tortuga.** El lenguaje propio se llama **Logo** en el juego,
   para no confundirlo con la app.
2. **Los lenguajes:** Bloques, Logo y Lua adentro de la app; **Python de
   instalación optativa** (sección 1.1).
3. **Girar:** `girar derecha` / `girar izquierda` (`turn right` / `turn
   left` en inglés; `girar_derecha()` en Lua y Python). `derecha` y `rt`
   quedan como sinónimos escondidos para quien venga del LOGO de siempre.
4. **Variables:** `cuenta = 0`, como en Lua y Python.
5. **Ganar depende del modo:** en el fácil, al pasar por la meta; en el
   difícil, terminando el programa justo en la meta (sección 3.2).
6. **Pistas:** con la tercera, el nivel queda en ★★ como mucho.
7. **La categoría de la Tienda:** el juego es de `programar` y de `juegos`
   a la vez. Hoy `cat` es una sola; poder poner dos es un pedido a la
   sesión del firmware (anotado en "Pedidos al firmware" de
   `HANDOFF-NUEVAS-APPS.md`). Mientras tanto, `programar`.

**Queda abierto:** para qué edad se escriben los textos de las tarjetas
(propuesta: desde los 8, con voseo y sin términos técnicos hasta el mundo
6).
