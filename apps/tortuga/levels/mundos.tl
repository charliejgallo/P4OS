The game's levels (DESIGN.md, 5). Format: DESIGN.md, 3.1. Every level
brings its Logo solution, the reference: tools/check_levels.sh runs it in
the C core and in the model, writes it as Lua and Python and runs those
too, on every map and in both modes, and checks that the pieces are the
three-star limit. Lines before the first level, and lines that start with
"@--", are comments. Maps are at most 11 x 8 tiles (they fit upright).

@-- ================================================= 1. Primeros pasos

@nivel 1-1
@titulo es: Derecho a la meta
@titulo en: Straight to the goal
@titulo de: Geradeaus zum Ziel
@carta es: ¡Hola! Soy la tortuga. Hago exactamente lo que dice tu programa, una orden detrás de otra.
@carta en: Hi! I am the turtle. I do exactly what your program says, one order after another.
@carta de: Hallo! Ich bin die Schildkröte. Ich mache genau, was dein Programm sagt, ein Befehl nach dem anderen.
@carta es: Armá el programa abajo y tocá Correr. Llevame hasta la bandera.
@carta en: Build the program below and tap Run. Take me to the flag.
@carta de: Bau unten das Programm und tipp auf Start. Bring mich zur Fahne.
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 1 4
@mapa
#######
#>...M#
#######
@pista es: La tortuga ya mira hacia la meta. ¿Cuántas casillas hay hasta ahí?
@pista en: The turtle already faces the goal. How many tiles away is it?
@pista de: Die Schildkröte schaut schon zum Ziel. Wie viele Felder sind es?
@pista es: Avanzar puede llevar un número: cuántas casillas de una vez.
@pista en: Going forward can take a number: how many tiles at once.
@pista de: Vorwärts kann eine Zahl haben: wie viele Felder auf einmal.
@logo
avanzar 4

@nivel 1-2
@titulo es: Doblar
@titulo en: Turning
@titulo de: Abbiegen
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 3 5
@mapa
######
#>..##
###.##
###M##
######
@pista es: Primero hasta la esquina, después girar, después bajar.
@pista en: First to the corner, then turn, then go down.
@pista de: Erst bis zur Ecke, dann drehen, dann nach unten.
@pista es: Para la tortuga, doblar hacia abajo es girar a la derecha.
@pista en: For the turtle, turning to go down is turning right.
@pista de: Für die Schildkröte heißt nach unten abbiegen: nach rechts drehen.
@logo
avanzar 2
girar derecha
avanzar 2

@nivel 1-3
@titulo es: La esquina
@titulo en: The corner
@titulo de: Die Ecke
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 5 7
@mapa
######
#>..##
###.##
###.M#
######
@pista es: Contá las casillas hasta cada esquina antes de doblar.
@pista en: Count the tiles to each corner before turning.
@pista de: Zähl die Felder bis zu jeder Ecke, bevor du abbiegst.
@pista es: Son dos giros, uno para cada lado.
@pista en: There are two turns, one to each side.
@pista de: Es sind zwei Drehungen, eine in jede Richtung.
@logo
avanzar 2
girar derecha
avanzar 2
girar izquierda
avanzar

@nivel 1-4
@titulo es: La primera lechuga
@titulo en: The first lettuce
@titulo de: Der erste Salat
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 3 4
@mapa
########
#>.L..M#
########
@pista es: Hay que parar justo sobre la lechuga y comerla antes de seguir.
@pista en: Stop right on the lettuce and eat it before going on.
@pista de: Halte genau auf dem Salat an und iss ihn, bevor es weitergeht.
@pista es: Avanzar, comer, avanzar: tres órdenes.
@pista en: Forward, eat, forward: three orders.
@pista de: Vorwärts, essen, vorwärts: drei Befehle.
@logo
avanzar 2
comer
avanzar 3

@nivel 1-5
@titulo es: La llave y la puerta
@titulo en: The key and the door
@titulo de: Schlüssel und Tür
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 3 4
@mapa
########
#>.k...#
######P#
######.#
######M#
########
@pista es: La llave se junta sola al pasar por encima.
@pista en: The key is picked up just by walking over it.
@pista de: Den Schlüssel nimmt sie einfach beim Drüberlaufen mit.
@pista es: Con la llave, la puerta se abre sola cuando la tortuga llega.
@pista en: With the key, the door opens by itself when the turtle gets there.
@pista de: Mit dem Schlüssel geht die Tür von selbst auf.
@logo
avanzar 5
girar derecha
avanzar 3

@nivel 1-6
@titulo es: El zigzag
@titulo en: The zigzag
@titulo de: Der Zickzack
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 9 12
@mapa
#########
#>.######
##.######
##...####
####.####
####...M#
#########
@pista es: Es una escalera: derecha, izquierda, derecha, izquierda.
@pista en: It is a staircase: right, left, right, left.
@pista de: Es ist eine Treppe: rechts, links, rechts, links.
@pista es: Mirá cuántas casillas tiene cada tramo antes de girar.
@pista en: Look how many tiles each stretch has before turning.
@pista de: Schau, wie viele Felder jedes Stück hat, bevor du drehst.
@logo
avanzar
girar derecha
avanzar 2
girar izquierda
avanzar 2
girar derecha
avanzar 2
girar izquierda
avanzar 3

@-- ======================================================= 2. Otra vez

@nivel 2-1
@titulo es: Lechugas en fila
@titulo en: Lettuce in a row
@titulo de: Salat in einer Reihe
@carta es: Cuando algo se repite, no hace falta escribirlo muchas veces: repetir lo hace por vos.
@carta en: When something repeats, you don't need to write it many times: repeat does it for you.
@carta de: Wenn sich etwas wiederholt, musst du es nicht oft schreiben: Wiederhole macht das für dich.
@carta es: Menos piezas, más estrellas.
@carta en: Fewer pieces, more stars.
@carta de: Weniger Teile, mehr Sterne.
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 4 7
@mapa
#########
#>LLLLLM#
#########
@pista es: Avanzar y comer, avanzar y comer... ¿cuántas veces?
@pista en: Forward and eat, forward and eat... how many times?
@pista de: Vorwärts und essen, vorwärts und essen... wie oft?
@pista es: Repetir hace lo de adentro las veces que le digas.
@pista en: Repeat does what is inside as many times as you say.
@pista de: Wiederhole macht das Innere so oft, wie du sagst.
@logo
repetir 5 [
  avanzar
  comer
]
avanzar

@nivel 2-2
@titulo es: La escalera
@titulo en: The staircase
@titulo de: Die Treppe
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 5 9
@mapa
#######
#####M#
####..#
###..##
##..###
#>.####
#######
@pista es: Mirá un escalón: avanzar, girar a la izquierda, avanzar, girar a la derecha.
@pista en: Look at one step: forward, turn left, forward, turn right.
@pista de: Schau dir eine Stufe an: vorwärts, links, vorwärts, rechts.
@pista es: Son cuatro escalones iguales.
@pista en: There are four equal steps.
@pista de: Es sind vier gleiche Stufen.
@logo
repetir 4 [
  avanzar
  girar izquierda
  avanzar
  girar derecha
]

@nivel 2-3
@titulo es: Escalera de lechugas
@titulo en: Lettuce staircase
@titulo de: Salattreppe
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 7 12
@mapa
########
#####.M#
####.L##
###.L###
##.L####
#>L#####
########
@pista es: Cada escalón tiene una lechuga. Lo que se repite es comer y subir.
@pista en: Every step has a lettuce. What repeats is eating and climbing.
@pista de: Jede Stufe hat einen Salat. Was sich wiederholt: essen und steigen.
@pista es: Después de los cuatro escalones falta un paso hasta la meta.
@pista en: After the four steps there is one more tile to the goal.
@pista de: Nach den vier Stufen fehlt noch ein Feld bis zum Ziel.
@logo
repetir 4 [
  avanzar
  comer
  girar izquierda
  avanzar
  girar derecha
]
avanzar

@nivel 2-4
@titulo es: La vuelta a la laguna
@titulo en: Around the pond
@titulo de: Um den Teich
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 4 7
@inicio 1 1 este
@mapa
######
#M..L#
#.~~.#
#.~~.#
#L..L#
######
@pista es: La meta está donde arrancás: hay que dar la vuelta entera.
@pista en: The goal is where you start: go all the way around.
@pista de: Das Ziel ist dort, wo du startest: einmal ganz herum.
@pista es: Cada lado es igual: avanzar 3, comer, girar.
@pista en: Every side is the same: forward 3, eat, turn.
@pista de: Jede Seite ist gleich: 3 vorwärts, essen, drehen.
@logo
repetir 4 [ avanzar 3 comer girar derecha ]

@nivel 2-5
@titulo es: El cerco de lechugas
@titulo en: The lettuce fence
@titulo de: Der Salatzaun
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 5 8
@inicio 1 1 este
@mapa
######
#MLLL#
#L~~L#
#L~~L#
#LLLL#
######
@pista es: Un lado es tres veces avanzar y comer. Y son cuatro lados.
@pista en: One side is forward-and-eat three times. And there are four sides.
@pista de: Eine Seite ist dreimal vorwärts und essen. Und es sind vier Seiten.
@pista es: Se puede poner un repetir adentro de otro.
@pista en: You can put one repeat inside another.
@pista de: Man kann ein Wiederhole in ein anderes stecken.
@logo
repetir 4 [
  repetir 3 [ avanzar comer ]
  girar derecha
]

@nivel 2-6
@titulo es: La serpiente
@titulo en: The snake
@titulo de: Die Schlange
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 10 17
@inicio 1 1 sur
@mapa
###########
#.#...#...#
#.#.#.#.#.#
#.#.#.#.#.#
#.#.#.#.#.#
#...#...#M#
###########
@pista es: Bajar, pasar, subir, pasar... y otra vez lo mismo.
@pista en: Down, across, up, across... and the same again.
@pista de: Runter, rüber, hoch, rüber... und dasselbe nochmal.
@pista es: Lo que se repite son ocho órdenes. Al final falta bajar una vez más.
@pista en: What repeats is eight orders. At the end, one more way down.
@pista de: Was sich wiederholt, sind acht Befehle. Am Ende noch einmal runter.
@logo
repetir 2 [
  avanzar 4
  girar izquierda
  avanzar 2
  girar izquierda
  avanzar 4
  girar derecha
  avanzar 2
  girar derecha
]
avanzar 4

@-- =================================================== 3. Ponele nombre

@nivel 3-1
@titulo es: Tres rincones
@titulo en: Three nooks
@titulo de: Drei Nischen
@carta es: Un truco es un grupo de órdenes con un nombre. Lo armás una vez y después lo usás cuando quieras.
@carta en: A trick is a group of orders with a name. You build it once and use it whenever you like.
@carta de: Ein Trick ist eine Gruppe von Befehlen mit Namen. Du baust ihn einmal und benutzt ihn, wann du willst.
@ordenes avanzar retroceder girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 13 19
@mapa
###########
###L#L##L##
#>.......M#
###########
@pista es: Entrar a un rincón, comer y volver: son siempre las mismas cinco órdenes.
@pista en: Into a nook, eat and back: always the same five orders.
@pista de: In die Nische, essen, zurück: immer dieselben fünf Befehle.
@pista es: Ponele un nombre a esas cinco órdenes y usalo tres veces.
@pista en: Give those five orders a name and use it three times.
@pista de: Gib diesen fünf Befehlen einen Namen und benutze ihn dreimal.
@logo
para rincon
  girar izquierda
  avanzar
  comer
  retroceder
  girar derecha
fin
avanzar 2
rincon
avanzar 2
rincon
avanzar 3
rincon
avanzar

@nivel 3-2
@titulo es: Dos piedras
@titulo en: Two rocks
@titulo de: Zwei Steine
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 13 17
@mapa
###########
##...#...##
#>.#...#.M#
###########
@pista es: Rodear una piedra lleva siete órdenes. ¿Y si le ponés un nombre?
@pista en: Going around a rock takes seven orders. What if you name them?
@pista de: Um einen Stein herum sind sieben Befehle. Und wenn du sie benennst?
@pista es: Entre las dos piedras hay distinta distancia: por eso no alcanza con repetir.
@pista en: The rocks are not equally far apart: that is why repeat is not enough.
@pista de: Die Steine sind nicht gleich weit weg: darum reicht Wiederhole nicht.
@logo
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

@nivel 3-3
@titulo es: El truco sirve para todos lados
@titulo en: The trick works any way
@titulo de: Der Trick geht in jede Richtung
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 15 19
@mapa
########
##...###
#>.#..##
#####..#
######.#
#####..#
#####M##
########
@pista es: Las dos piedras se rodean igual, aunque la tortuga mire para otro lado.
@pista en: Both rocks are rounded the same way, even facing another way.
@pista de: Beide Steine umrundet man gleich, auch in eine andere Richtung.
@pista es: Las órdenes de girar son para la tortuga, no para el mapa.
@pista en: Turning is the turtle's left and right, not the map's.
@pista de: Drehen ist links und rechts der Schildkröte, nicht der Karte.
@logo
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
avanzar
girar derecha
avanzar
rodear
avanzar

@nivel 3-4
@titulo es: Las dos lagunas
@titulo en: The two ponds
@titulo de: Die zwei Teiche
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 12 14
@inicio 1 3 norte
@mapa
#########
#L.L#L.L#
#.~.#.~.#
#L.L.L.LM
#########
@pista es: Cada laguna tiene una lechuga en cada esquina. La vuelta es igual en las dos.
@pista en: Each pond has a lettuce on every corner. The lap is the same at both.
@pista de: Jeder Teich hat an jeder Ecke einen Salat. Die Runde ist bei beiden gleich.
@pista es: Un truco con un repetir adentro, y lo usás dos veces.
@pista en: A trick with a repeat inside, used twice.
@pista de: Ein Trick mit einem Wiederhole darin, zweimal benutzt.
@logo
para vuelta
  repetir 4 [ avanzar 2 comer girar derecha ]
fin
vuelta
girar derecha
avanzar 4
girar izquierda
vuelta
girar derecha
avanzar 3

@nivel 3-5
@titulo es: Dos trucos
@titulo en: Two tricks
@titulo de: Zwei Tricks
@ordenes avanzar retroceder girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 22 32
@mapa
###########
##...#....#
#>.#...#.M#
##L#L###L##
###########
@pista es: Hay piedras para rodear y rincones con lechuga: un truco para cada cosa.
@pista en: Rocks to go around and nooks with lettuce: a trick for each.
@pista de: Steine zum Umgehen und Nischen mit Salat: ein Trick für jedes.
@pista es: Los rincones están abajo: el truco gira a la derecha para entrar.
@pista en: The nooks are below: the trick turns right to go in.
@pista de: Die Nischen sind unten: der Trick dreht nach rechts hinein.
@logo
para rodear
  girar izquierda
  avanzar
  girar derecha
  avanzar 2
  girar derecha
  avanzar
  girar izquierda
fin
para rincon
  girar derecha
  avanzar
  comer
  retroceder
  girar izquierda
fin
avanzar
rincon
rodear
rincon
avanzar 2
rodear
rincon
avanzar

@-- ======================================================= 4. Preguntar

@nivel 4-1
@titulo es: Lechugas salteadas
@titulo en: Scattered lettuce
@titulo de: Verstreuter Salat
@carta es: Desde ahora tu programa se prueba en varios caminos a la vez, y tiene que andar en todos.
@carta en: From now on your program is tried on several paths at once, and it has to work on all of them.
@carta de: Ab jetzt läuft dein Programm auf mehreren Wegen gleichzeitig und muss auf allen klappen.
@carta es: Para eso la tortuga puede preguntar: si pasa algo, hace una cosa; si no, otra.
@carta en: For that the turtle can ask: if something is so, do one thing; if not, another.
@carta de: Dafür kann die Schildkröte fragen: Wenn etwas so ist, tu das eine; sonst das andere.
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
@mapa
##########
#>...L..M#
##########
@pista es: Tu programa se prueba en tres caminos distintos: no sirve contar dónde están.
@pista en: Your program is tried on three different paths: counting where they are won't do.
@pista de: Dein Programm läuft auf drei Wegen: Abzählen, wo sie liegen, hilft nicht.
@pista es: En cada casilla, preguntá si hay lechuga, y si hay, comela.
@pista en: On every tile, ask if there is lettuce, and if there is, eat it.
@pista de: Frag auf jedem Feld, ob Salat da ist, und wenn ja, iss ihn.
@logo
repetir 7 [
  avanzar
  si hay_lechuga [ comer ]
]

@nivel 4-2
@titulo es: ¿Por dónde sigue?
@titulo en: Which way now?
@titulo de: Wo geht es weiter?
@ordenes avanzar girar_derecha girar_izquierda camino_adelante
@meta llegar
@estrellas 5 7
@mapa
#########
#>...####
####.####
####.####
####.####
####.####
####M####
#########
@mapa
#########
#>......#
#######.#
#######M#
#########
@mapa
#########
#>.....##
######.##
######.##
######M##
#########
@pista es: Los caminos tienen ocho casillas, pero doblan en lugares distintos.
@pista en: The paths are eight tiles long, but turn in different places.
@pista de: Die Wege sind acht Felder lang, biegen aber an anderen Stellen ab.
@pista es: Si hay camino adelante, avanzá; si no, girá y avanzá. Ocho veces.
@pista en: If there is a path ahead, go; if not, turn and go. Eight times.
@pista de: Wenn vorne ein Weg ist, geh; wenn nicht, dreh dich und geh. Achtmal.
@logo
repetir 8 [
  si camino_adelante [ avanzar ] sino [ girar derecha avanzar ]
]

@nivel 4-3
@titulo es: Charcos
@titulo en: Puddles
@titulo de: Pfützen
@ordenes avanzar girar_derecha girar_izquierda agua_adelante
@meta llegar
@estrellas 12 16
@mapa
###########
#...#...###
#>~...~..M#
###########
@mapa
###########
###...#...#
#>..~...~M#
###########
@mapa
###########
#.........#
#>~.~.~.~M#
###########
@pista es: El camino va de a dos casillas. Si adelante hay agua, rodeala; si no, avanzá dos.
@pista en: The path goes two tiles at a time. If there is water ahead, go around; if not, go two.
@pista de: Der Weg geht in Zweierschritten. Ist vorne Wasser, umgehe es; sonst zwei vorwärts.
@pista es: Rodear el charco es un truco con nombre, como en el mundo 3.
@pista en: Going around the puddle is a named trick, like in world 3.
@pista de: Um die Pfütze herum ist ein Trick mit Namen, wie in Welt 3.
@logo
para rodear
  girar izquierda
  avanzar
  girar derecha
  avanzar 2
  girar derecha
  avanzar
  girar izquierda
fin
repetir 4 [
  si agua_adelante [ rodear ] sino [ avanzar 2 ]
]

@nivel 4-4
@titulo es: ¿Dónde está el puente?
@titulo en: Where is the bridge?
@titulo de: Wo ist die Brücke?
@ordenes avanzar girar_derecha girar_izquierda agua_adelante
@meta llegar
@estrellas 6 8
@mapa
#######
##v####
##..###
~~=~~~~
#.....#
##M...#
#######
@mapa
#######
##v####
##..###
~~~=~~~
#.....#
###M..#
#######
@mapa
#######
##v####
##..###
~~=~~~~
##.####
##M####
#######
@pista es: A veces el puente está justo adelante, a veces una casilla más allá.
@pista en: Sometimes the bridge is right ahead, sometimes one tile over.
@pista de: Manchmal ist die Brücke direkt vorne, manchmal ein Feld daneben.
@pista es: Si adelante hay agua, correte una casilla antes de cruzar.
@pista en: If there is water ahead, move over one tile before crossing.
@pista de: Ist vorne Wasser, geh vor dem Überqueren ein Feld zur Seite.
@logo
avanzar
si agua_adelante [
  girar izquierda
  avanzar
  girar derecha
]
avanzar 3

@nivel 4-5
@titulo es: Charcos y lechugas
@titulo en: Puddles and lettuce
@titulo de: Pfützen und Salat
@ordenes avanzar girar_derecha girar_izquierda comer agua_adelante hay_lechuga
@meta lechugas llegar
@estrellas 14 18
@mapa
###########
#...#...###
#>~L..~L.M#
###########
@mapa
###########
###...#...#
#>..~L..~M#
###########
@mapa
###########
#.........#
#>~L~L~L~M#
###########
@pista es: Como en Charcos, pero al final de cada tramo puede haber una lechuga.
@pista en: Like Puddles, but at the end of each stretch there may be a lettuce.
@pista de: Wie bei Pfützen, aber am Ende jedes Stücks kann Salat liegen.
@pista es: Dos preguntas en cada vuelta: ¿hay agua? y, después de moverte, ¿hay lechuga?
@pista en: Two questions each time: is there water? and, after moving, is there lettuce?
@pista de: Zwei Fragen jedes Mal: Ist da Wasser? und nach dem Gehen: Ist da Salat?
@logo
para rodear
  girar izquierda
  avanzar
  girar derecha
  avanzar 2
  girar derecha
  avanzar
  girar izquierda
fin
repetir 4 [
  si agua_adelante [ rodear ] sino [ avanzar 2 ]
  si hay_lechuga [ comer ]
]

@nivel 4-6
@titulo es: Para los dos lados
@titulo en: Both ways
@titulo de: In beide Richtungen
@ordenes avanzar girar_derecha girar_izquierda camino_adelante camino_derecha
@meta llegar
@estrellas 7 9
@mapa
#########
#>...####
####.####
####...M#
#########
@mapa
########
###...M#
###.####
###.####
#>..####
########
@mapa
########
#>.....#
######.#
######.#
######M#
########
@pista es: Ahora los caminos doblan a la derecha o a la izquierda.
@pista en: Now the paths turn right or left.
@pista de: Jetzt biegen die Wege nach rechts oder links ab.
@pista es: Si no hay camino adelante, preguntá si hay a la derecha; si no, es a la izquierda.
@pista en: If there is no path ahead, ask about the right; if not, it is the left.
@pista de: Ist vorne kein Weg, frag nach rechts; wenn nicht, ist es links.
@logo
repetir 8 [
  si camino_adelante [
    avanzar
  ] sino [
    si camino_derecha [ girar derecha ] sino [ girar izquierda ]
    avanzar
  ]
]

@-- ======================================================= 5. Hasta que

@nivel 5-1
@titulo es: Hasta la pared
@titulo en: Up to the wall
@titulo de: Bis zur Wand
@carta es: «Mientras» repite hasta que algo cambia. No hace falta saber cuántas veces.
@carta en: «While» repeats until something changes. You don't need to know how many times.
@carta de: «Solange» wiederholt, bis sich etwas ändert. Du musst nicht wissen, wie oft.
@ordenes avanzar camino_adelante
@meta llegar
@estrellas 2 3
@mapa
######
#>..M#
######
@mapa
#########
#>.....M#
#########
@mapa
###########
#>.......M#
###########
@pista es: Cada camino es de otro largo. No hace falta contar.
@pista en: Each path has another length. No need to count.
@pista de: Jeder Weg ist anders lang. Zählen ist nicht nötig.
@pista es: Mientras haya camino adelante, avanzá.
@pista en: While there is a path ahead, go forward.
@pista de: Solange vorne ein Weg ist, geh vorwärts.
@logo
mientras camino_adelante [ avanzar ]

@nivel 5-2
@titulo es: Lechugas hasta la meta
@titulo en: Lettuce up to the goal
@titulo de: Salat bis zum Ziel
@ordenes avanzar comer hay_lechuga en_meta
@meta lechugas llegar
@estrellas 4 6
@mapa
########
#>.L.LM#
########
@mapa
###########
#>LL.L..LM#
###########
@mapa
########
#>..L.M#
########
@pista es: No sabés cuánto mide el camino ni dónde están las lechugas.
@pista en: You don't know how long the path is or where the lettuce is.
@pista de: Du weißt nicht, wie lang der Weg ist und wo der Salat liegt.
@pista es: Mientras no estés en la meta: avanzá, y si hay lechuga, comela.
@pista en: While you are not at the goal: go, and if there is lettuce, eat it.
@pista de: Solange du nicht am Ziel bist: geh, und wenn Salat da ist, iss ihn.
@logo
mientras no en_meta [
  avanzar
  si hay_lechuga [ comer ]
]

@nivel 5-3
@titulo es: Hasta el fondo
@titulo en: All the way
@titulo de: Bis ganz hinten
@ordenes avanzar girar_derecha girar_izquierda camino_adelante en_meta
@meta llegar
@estrellas 4 5
@mapa
#######
#>...##
####.##
####.##
####M##
#######
@mapa
#########
#>......#
#######.#
#######M#
#########
@mapa
####
#>.#
##.#
##.#
##.#
##M#
####
@pista es: No sabés cuánto mide cada tramo: avanzá mientras haya camino.
@pista en: You don't know how long each stretch is: go while there is a path.
@pista de: Du weißt nicht, wie lang jedes Stück ist: geh, solange ein Weg da ist.
@pista es: Hay una forma de hacerlo con una sola vuelta que pregunta y gira.
@pista en: There is a way with a single loop that asks and turns.
@pista de: Es geht mit einer einzigen Schleife, die fragt und dreht.
@logo
mientras no en_meta [
  si camino_adelante [ avanzar ] sino [ girar derecha ]
]

@nivel 5-4
@titulo es: El laberinto
@titulo en: The maze
@titulo de: Das Labyrinth
@ordenes avanzar girar_derecha girar_izquierda camino_adelante camino_derecha en_meta
@meta llegar
@estrellas 7 9
@mapa
#########
#>..#...#
###.#.#.#
#...#.#.#
#.###.#.#
#.....#M#
#########
@mapa
#########
#>......#
#.#####.#
#.#M..#.#
#.###.#.#
#.....#.#
#######.#
#.......#
#########
@pista es: Una regla vieja de los laberintos: andá siempre con la mano derecha tocando la pared.
@pista en: An old maze rule: always walk with your right hand on the wall.
@pista de: Eine alte Labyrinth-Regel: immer mit der rechten Hand an der Wand gehen.
@pista es: Si hay camino a la derecha, doblá y avanzá; si no, si hay adelante, avanzá; si no, girá a la izquierda.
@pista en: If there is a path on the right, turn and go; else if ahead, go; else turn left.
@pista de: Ist rechts ein Weg, dreh und geh; sonst wenn vorne, geh; sonst dreh nach links.
@logo
mientras no en_meta [
  si camino_derecha [
    girar derecha
    avanzar
  ] sino [
    si camino_adelante [ avanzar ] sino [ girar izquierda ]
  ]
]

@nivel 5-5
@titulo es: Escalera sin contar
@titulo en: Stairs without counting
@titulo de: Treppe ohne Zählen
@ordenes avanzar girar_derecha girar_izquierda en_meta
@meta llegar
@estrellas 5 7
@mapa
######
####M#
###..#
##..##
#>.###
######
@mapa
########
######M#
#####..#
####..##
###..###
##..####
#>.#####
########
@mapa
#####
###M#
##..#
#>.##
#####
@pista es: Las escaleras tienen distinta cantidad de escalones.
@pista en: The stairs have different numbers of steps.
@pista de: Die Treppen haben verschieden viele Stufen.
@pista es: Mientras no estés en la meta, subí un escalón más.
@pista en: While you are not at the goal, climb one more step.
@pista de: Solange du nicht am Ziel bist, steig noch eine Stufe.
@logo
mientras no en_meta [
  avanzar
  girar izquierda
  avanzar
  girar derecha
]

@nivel 5-6
@titulo es: El laberinto grande
@titulo en: The big maze
@titulo de: Das große Labyrinth
@ordenes avanzar girar_derecha girar_izquierda camino_adelante camino_derecha en_meta
@meta llegar
@estrellas 7 9
@mapa
###########
#>..#.....#
###.#.###.#
#...#.#...#
#.###.#.###
#.....#..M#
###########
@mapa
###########
#>......#.#
#.#####.#.#
#.#...#...#
#.#.#.#####
#...#....M#
###########
@pista es: La regla de la mano derecha sirve en cualquier laberinto como estos.
@pista en: The right-hand rule works in any maze like these.
@pista de: Die Rechte-Hand-Regel klappt in jedem Labyrinth wie diesen.
@pista es: Es el mismo programa que en El laberinto.
@pista en: It is the same program as in The maze.
@pista de: Es ist dasselbe Programm wie in Das Labyrinth.
@logo
mientras no en_meta [
  si camino_derecha [
    girar derecha
    avanzar
  ] sino [
    si camino_adelante [ avanzar ] sino [ girar izquierda ]
  ]
]

@-- ========================================================== 6. Contar

@nivel 6-1
@titulo es: Contá las lechugas
@titulo en: Count the lettuce
@titulo de: Zähl den Salat
@carta es: Una variable es una cajita con un nombre que guarda un número. Lo podés cambiar y usar después.
@carta en: A variable is a little box with a name that keeps a number. You can change it and use it later.
@carta de: Eine Variable ist ein Kästchen mit Namen, das eine Zahl speichert. Du kannst sie ändern und später benutzen.
@ordenes avanzar decir hay_lechuga
@meta llegar decir=lechugas
@estrellas 6 8
@mapa
###########
#>.L..L.LM#
###########
@mapa
###########
#>LL.....M#
###########
@mapa
###########
#>.L.LL.LM#
###########
@pista es: Al final la tortuga tiene que decir cuántas lechugas vio.
@pista en: At the end the turtle has to say how many lettuces it saw.
@pista de: Am Ende muss die Schildkröte sagen, wie viele Salate sie gesehen hat.
@pista es: Una variable que arranque en 0 y sume uno por cada lechuga.
@pista en: A variable that starts at 0 and adds one for each lettuce.
@pista de: Eine Variable, die bei 0 anfängt und für jeden Salat eins dazuzählt.
@logo
cuenta = 0
repetir 8 [
  avanzar
  si hay_lechuga [ cuenta += 1 ]
]
decir cuenta

@nivel 6-2
@titulo es: Contar hasta la pared
@titulo en: Counting up to the wall
@titulo de: Zählen bis zur Wand
@ordenes avanzar decir hay_lechuga camino_adelante
@meta llegar decir=lechugas
@estrellas 6 8
@mapa
#########
#>.L..LM#
#########
@mapa
##########
#>L.L.LLM#
##########
@mapa
########
#>..L.M#
########
@pista es: Ahora tampoco sabés cuánto mide el camino.
@pista en: Now you don't know how long the path is either.
@pista de: Jetzt weißt du auch nicht, wie lang der Weg ist.
@pista es: Mientras haya camino: avanzar y, si hay lechuga, sumar uno. Después, decir.
@pista en: While there is a path: go and, if there is lettuce, add one. Then say it.
@pista de: Solange ein Weg da ist: gehen und bei Salat eins dazu. Dann sagen.
@logo
cuenta = 0
mientras camino_adelante [
  avanzar
  si hay_lechuga [ cuenta += 1 ]
]
decir cuenta

@nivel 6-3
@titulo es: La espiral de pasos
@titulo en: The spiral of steps
@titulo de: Die Schrittspirale
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 5 8
@mapa
#########
#......M#
#.#######
#.#...###
#.#^#.###
#.###.###
#.....###
#########
@pista es: Cada tramo es una casilla más largo que el anterior: 1, 2, 3...
@pista en: Each stretch is one tile longer than the one before: 1, 2, 3...
@pista de: Jedes Stück ist ein Feld länger als das davor: 1, 2, 3...
@pista es: Guardá el largo en una variable y sumale uno después de cada giro.
@pista en: Keep the length in a variable and add one after each turn.
@pista de: Speicher die Länge in einer Variable und zähl nach jeder Drehung eins dazu.
@logo
lado = 1
repetir 6 [
  avanzar lado
  girar derecha
  lado += 1
]

@nivel 6-4
@titulo es: La espiral hacia adentro
@titulo en: The spiral inwards
@titulo de: Die Spirale nach innen
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 5 9
@mapa
########
#>.....#
######.#
###.M#.#
###.##.#
###....#
########
@pista es: Ahora cada tramo es una casilla más corto: 5, 4, 3...
@pista en: Now each stretch is one tile shorter: 5, 4, 3...
@pista de: Jetzt ist jedes Stück ein Feld kürzer: 5, 4, 3...
@pista es: La variable arranca en 5 y le restás uno en cada vuelta.
@pista en: The variable starts at 5 and you take one away each time.
@pista de: Die Variable startet bei 5 und du ziehst jedes Mal eins ab.
@logo
lado = 5
repetir 5 [
  avanzar lado
  girar derecha
  lado -= 1
]

@-- ======================================================== 7. A medida

@nivel 7-1
@titulo es: Filas a medida
@titulo en: Rows of any size
@titulo de: Reihen nach Maß
@carta es: Los trucos pueden recibir un dato, como un número: así el mismo truco sirve para tamaños distintos.
@carta en: Tricks can take a value, like a number: that way the same trick works for different sizes.
@carta de: Tricks können einen Wert bekommen, etwa eine Zahl: So passt derselbe Trick für verschiedene Größen.
@ordenes avanzar girar_derecha girar_izquierda comer
@meta lechugas llegar
@estrellas 10 12
@mapa
##########
#>LLL#####
####L#####
####LLLLM#
##########
@pista es: Hay filas de lechugas de distinto largo: 3, 2 y 3.
@pista en: There are rows of lettuce of different lengths: 3, 2 and 3.
@pista de: Es gibt Salatreihen verschiedener Länge: 3, 2 und 3.
@pista es: Un truco que reciba el largo: para fila n, repetir n veces avanzar y comer.
@pista en: A trick that takes the length: to row n, repeat n times forward and eat.
@pista de: Ein Trick mit der Länge: für Reihe n, n-mal vorwärts und essen.
@logo
para fila n
  repetir n [ avanzar comer ]
fin
fila 3
girar derecha
fila 2
girar izquierda
fila 3
avanzar

@nivel 7-2
@titulo es: Escalones de todos los tamaños
@titulo en: Steps of every size
@titulo de: Stufen in allen Größen
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 8 11
@mapa
#########
#######M#
#######.#
#######.#
####....#
####.####
##...####
#>.######
#########
@pista es: Un escalón de tamaño n: avanzar n, girar a la izquierda, avanzar n, girar a la derecha.
@pista en: A step of size n: forward n, turn left, forward n, turn right.
@pista de: Eine Stufe der Größe n: n vorwärts, links, n vorwärts, rechts.
@pista es: Después lo usás con 1, con 2 y con 3.
@pista en: Then you use it with 1, with 2 and with 3.
@pista de: Dann benutzt du ihn mit 1, mit 2 und mit 3.
@logo
para escalon n
  avanzar n
  girar izquierda
  avanzar n
  girar derecha
fin
escalon 1
escalon 2
escalon 3

@nivel 7-3
@titulo es: Rodear a medida
@titulo en: Going around any size
@titulo de: Umgehen nach Maß
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 13 17
@mapa
###########
##...#....#
#>.#...##.M
###########
@pista es: Una piedra es de una casilla y la otra de dos.
@pista en: One rock is one tile wide and the other is two.
@pista de: Ein Stein ist ein Feld breit, der andere zwei.
@pista es: El truco rodear recibe el ancho, y arriba avanza el ancho más uno.
@pista en: The trick takes the width, and up top it goes the width plus one.
@pista de: Der Trick bekommt die Breite und geht oben Breite plus eins.
@logo
para rodear n
  girar izquierda
  avanzar
  girar derecha
  avanzar n + 1
  girar derecha
  avanzar
  girar izquierda
fin
avanzar
rodear 1
avanzar 2
rodear 2
avanzar

@-- ========================================================= 8. El lápiz

@nivel 8-1
@titulo es: El cuadrado
@titulo en: The square
@titulo de: Das Quadrat
@carta es: Ahora estás sobre una hoja: la tortuga deja un trazo por donde pasa.
@carta en: Now you are on a sheet of paper: the turtle leaves a line wherever it goes.
@carta de: Jetzt bist du auf einem Blatt Papier: Die Schildkröte zieht überall eine Linie.
@carta es: Avanza en pasos chicos y gira los grados que quieras. Dibujá la figura de puntitos.
@carta en: It moves in small steps and turns any number of degrees. Draw the dotted figure.
@carta de: Sie geht kleine Schritte und dreht beliebig viele Grad. Zeichne die gepunktete Figur.
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 3 9
@inicio -50 -50 norte
@figura
avanzar 100 girar derecha 90 avanzar 100 girar derecha 90
avanzar 100 girar derecha 90 avanzar 100
@pista es: En la hoja la tortuga avanza pasos y gira grados. El lápiz ya está bajado.
@pista en: On the sheet the turtle moves in steps and turns in degrees. The pen is already down.
@pista de: Auf dem Blatt geht die Schildkröte Schritte und dreht Grad. Der Stift ist schon unten.
@pista es: Cuatro lados de 100 y, en cada esquina, un giro de 90.
@pista en: Four sides of 100 and, at each corner, a turn of 90.
@pista de: Vier Seiten zu 100 und an jeder Ecke eine Drehung um 90.
@logo
repetir 4 [ avanzar 100 girar derecha 90 ]

@nivel 8-2
@titulo es: El triángulo
@titulo en: The triangle
@titulo de: Das Dreieck
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 3 6
@inicio -60 -50 norte
@figura
avanzar 120 girar derecha 120 avanzar 120 girar derecha 120 avanzar 120
@pista es: Tres lados iguales. ¿Cuánto hay que girar en cada punta?
@pista en: Three equal sides. How much to turn at each corner?
@pista de: Drei gleiche Seiten. Wie viel an jeder Ecke drehen?
@pista es: En total la tortuga da una vuelta entera, 360 grados, en tres giros.
@pista en: In all the turtle turns all the way round, 360 degrees, in three turns.
@pista de: Insgesamt dreht sich die Schildkröte einmal ganz, 360 Grad, in drei Drehungen.
@logo
repetir 3 [ avanzar 120 girar derecha 120 ]

@nivel 8-3
@titulo es: La estrella
@titulo en: The star
@titulo de: Der Stern
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 3 11
@inicio -75 -50 este
@figura
avanzar 150 girar derecha 144 avanzar 150 girar derecha 144 avanzar 150
girar derecha 144 avanzar 150 girar derecha 144 avanzar 150
@pista es: Cinco puntas, cinco lados iguales.
@pista en: Five points, five equal sides.
@pista de: Fünf Zacken, fünf gleiche Seiten.
@pista es: En total la tortuga da dos vueltas enteras: 720 grados en cinco giros.
@pista en: In all the turtle turns round twice: 720 degrees in five turns.
@pista de: Insgesamt dreht sich die Schildkröte zweimal: 720 Grad in fünf Drehungen.
@logo
repetir 5 [ avanzar 150 girar derecha 144 ]

@nivel 8-4
@titulo es: La casita
@titulo en: The little house
@titulo de: Das Häuschen
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 8 13
@inicio -50 -80 norte
@figura
avanzar 100 girar derecha 90 avanzar 100 girar derecha 90 avanzar 100 girar derecha 90 avanzar 100
girar derecha 90 avanzar 100 girar derecha 30 avanzar 100 girar derecha 120 avanzar 100
@pista es: Primero el cuadrado, después el techo arriba.
@pista en: First the square, then the roof on top.
@pista de: Erst das Quadrat, dann das Dach oben drauf.
@pista es: El techo es un triángulo: subí por la pared, girá 30 y hacé dos lados.
@pista en: The roof is a triangle: go up the wall, turn 30 and draw two sides.
@pista de: Das Dach ist ein Dreieck: die Wand hoch, um 30 drehen, zwei Seiten.
@logo
repetir 4 [ avanzar 100 girar derecha 90 ]
avanzar 100
girar derecha 30
repetir 2 [ avanzar 100 girar derecha 120 ]

@nivel 8-5
@titulo es: Polígonos
@titulo en: Polygons
@titulo de: Vielecke
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 8 12
@inicio -60 -60 norte
@figura
repetir 3 [ avanzar 60 girar derecha 120 ]
repetir 4 [ avanzar 60 girar derecha 90 ]
repetir 5 [ avanzar 60 girar derecha 72 ]
repetir 6 [ avanzar 60 girar derecha 60 ]
@pista es: Un triángulo, un cuadrado, un pentágono y un hexágono, uno sobre otro.
@pista en: A triangle, a square, a pentagon and a hexagon, one over another.
@pista de: Ein Dreieck, ein Quadrat, ein Fünfeck und ein Sechseck übereinander.
@pista es: Un truco poligono que reciba los lados: gira 360 dividido los lados.
@pista en: A polygon trick that takes the sides: it turns 360 divided by the sides.
@pista de: Ein Vieleck-Trick mit der Seitenzahl: er dreht 360 geteilt durch die Seiten.
@logo
para poligono lados
  repetir lados [ avanzar 60 girar derecha 360 / lados ]
fin
poligono 3
poligono 4
poligono 5
poligono 6

@nivel 8-6
@titulo es: La espiral
@titulo en: The spiral
@titulo de: Die Spirale
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 5 41
@inicio 0 0 norte
@figura
avanzar 10 girar derecha 90 avanzar 20 girar derecha 90 avanzar 30 girar derecha 90
avanzar 40 girar derecha 90 avanzar 50 girar derecha 90 avanzar 60 girar derecha 90
avanzar 70 girar derecha 90 avanzar 80 girar derecha 90 avanzar 90 girar derecha 90
avanzar 100 girar derecha 90 avanzar 110 girar derecha 90 avanzar 120 girar derecha 90
avanzar 130 girar derecha 90 avanzar 140 girar derecha 90 avanzar 150 girar derecha 90
avanzar 160 girar derecha 90 avanzar 170 girar derecha 90 avanzar 180 girar derecha 90
avanzar 190 girar derecha 90 avanzar 200
@pista es: Cada lado es 10 más largo que el anterior.
@pista en: Each side is 10 longer than the one before.
@pista de: Jede Seite ist 10 länger als die davor.
@pista es: Guardá el largo en una variable: empieza en 10 y suma 10 en cada vuelta.
@pista en: Keep the length in a variable: it starts at 10 and adds 10 each time.
@pista de: Speicher die Länge in einer Variable: Start 10, jedes Mal plus 10.
@logo
lado = 10
repetir 20 [
  avanzar lado
  girar derecha 90
  lado += 10
]

@nivel 8-7
@titulo es: La flor
@titulo en: The flower
@titulo de: Die Blume
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 7 10
@inicio 0 0 norte
@figura
repetir 12 [ repetir 6 [ avanzar 50 girar derecha 60 ] girar derecha 30 ]
@pista es: Doce hexágonos, cada uno girado 30 grados del anterior.
@pista en: Twelve hexagons, each turned 30 degrees from the one before.
@pista de: Zwölf Sechsecke, jedes um 30 Grad zum vorigen gedreht.
@pista es: Primero enseñale a dibujar un polígono de cualquier cantidad de lados.
@pista en: First teach it to draw a polygon with any number of sides.
@pista de: Bring ihr zuerst ein Vieleck mit beliebig vielen Seiten bei.
@logo
para poligono lados largo
  repetir lados [ avanzar largo girar derecha 360 / lados ]
fin
repetir 12 [ poligono 6 50 girar derecha 30 ]

@nivel 8-8
@titulo es: El árbol
@titulo en: The tree
@titulo de: Der Baum
@ordenes avanzar retroceder girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 11 15
@inicio 0 -170 norte
@figura
para arbol largo
  si largo < 10 [ alto ]
  avanzar largo
  girar izquierda 30
  arbol largo * 0.7
  girar derecha 60
  arbol largo * 0.7
  girar izquierda 30
  retroceder largo
fin
arbol 60
@pista es: Un árbol es un tronco con dos árboles más chicos arriba.
@pista en: A tree is a trunk with two smaller trees on top.
@pista de: Ein Baum ist ein Stamm mit zwei kleineren Bäumen obendrauf.
@pista es: El truco arbol se usa a sí mismo, con un largo cada vez más chico, y para cuando es muy corto.
@pista en: The tree trick uses itself, smaller each time, and stops when it is very short.
@pista de: Der Baum-Trick ruft sich selbst auf, jedes Mal kleiner, und hört auf, wenn er sehr kurz ist.
@logo
para arbol largo
  si largo < 10 [ alto ]
  avanzar largo
  girar izquierda 30
  arbol largo * 0.7
  girar derecha 60
  arbol largo * 0.7
  girar izquierda 30
  retroceder largo
fin
arbol 60

@-- ================================================ 9. La gran travesía

@nivel 9-1
@titulo es: El laberinto de las lechugas
@titulo en: The lettuce maze
@titulo de: Das Salatlabyrinth
@carta es: La gran travesía: niveles que piden todo lo que aprendiste, junto.
@carta en: The great journey: levels that ask for everything you have learned, together.
@carta de: Die große Reise: Level, die alles verlangen, was du gelernt hast, zusammen.
@ordenes avanzar girar_derecha girar_izquierda comer camino_adelante camino_derecha en_meta hay_lechuga
@meta lechugas llegar
@estrellas 9 11
@mapa
#########
#>.L#...#
###.#L#.#
#.L.#.#.#
#.###.#L#
#...L.#M#
#########
@mapa
###########
#>......#.#
#.#####.#.#
#L#L..#...#
#.#.#.#####
#...#L.L.M#
###########
@pista es: La mano derecha te lleva por todo el laberinto. En el camino hay lechugas.
@pista en: The right hand takes you through the whole maze. There is lettuce on the way.
@pista de: Die rechte Hand führt dich durch das ganze Labyrinth. Unterwegs liegt Salat.
@pista es: Al programa del laberinto sumale una pregunta: si hay lechuga, comela.
@pista en: Add one question to the maze program: if there is lettuce, eat it.
@pista de: Ergänze das Labyrinthprogramm um eine Frage: Wenn Salat da ist, iss ihn.
@logo
mientras no en_meta [
  si hay_lechuga [ comer ]
  si camino_derecha [
    girar derecha
    avanzar
  ] sino [
    si camino_adelante [ avanzar ] sino [ girar izquierda ]
  ]
]

@nivel 9-2
@titulo es: Charcos sin contar
@titulo en: Puddles without counting
@titulo de: Pfützen ohne Zählen
@ordenes avanzar girar_derecha girar_izquierda agua_adelante en_meta
@meta llegar
@estrellas 12 14
@mapa
##########
##......##
#>.~..~.M#
##########
@mapa
###########
#...#.....#
#>~...~.~M#
###########
@mapa
###########
###...#####
##>.~....M#
###########
@pista es: Los caminos son de distinto largo y los charcos están en cualquier lado.
@pista en: The paths have different lengths and the puddles are anywhere.
@pista de: Die Wege sind verschieden lang und die Pfützen liegen irgendwo.
@pista es: Mientras no llegues: si hay agua, rodeala; si no, avanzá una.
@pista en: Until you get there: if there is water, go around; if not, go one.
@pista de: Bis du ankommst: Ist da Wasser, umgehe es; sonst geh eins.
@logo
para rodear
  girar izquierda
  avanzar
  girar derecha
  avanzar 2
  girar derecha
  avanzar
  girar izquierda
fin
mientras no en_meta [
  si agua_adelante [ rodear ] sino [ avanzar ]
]

@nivel 9-3
@titulo es: El molinete
@titulo en: The pinwheel
@titulo de: Das Windrad
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 7 10
@inicio 0 0 norte
@figura
repetir 8 [ repetir 4 [ avanzar 80 girar derecha 90 ] girar derecha 45 ]
@pista es: Ocho cuadrados que giran alrededor del mismo punto.
@pista en: Eight squares turning around the same point.
@pista de: Acht Quadrate, die sich um denselben Punkt drehen.
@pista es: Un truco cuadrado, y entre cuadrado y cuadrado, un giro de 45.
@pista en: A square trick, and between one square and the next, a turn of 45.
@pista de: Ein Quadrat-Trick, und zwischen den Quadraten eine Drehung um 45.
@logo
para cuadrado lado
  repetir 4 [ avanzar lado girar derecha 90 ]
fin
repetir 8 [
  cuadrado 80
  girar derecha 45
]
