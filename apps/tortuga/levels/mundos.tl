The sample levels of DESIGN.md, section 7. Format: DESIGN.md, section 3.
Lines before the first level, and lines that start with "@--", are
comments. The solutions are the
reference ones: tools/tg_ref.py runs each on every map and checks that the
three languages count the same pieces as the three-star limit.

@nivel 1-3
@titulo es: La esquina
@titulo en: The corner
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 5 7
@mapa
######
#>..##
###.##
###.M#
######
@pista es: Contá las casillas hasta la esquina antes de doblar.
@logo
avanzar 2
girar derecha
avanzar 2
girar izquierda
avanzar
@lua
avanzar(2)
girar_derecha()
avanzar(2)
girar_izquierda()
avanzar()
@python
avanzar(2)
girar_derecha()
avanzar(2)
girar_izquierda()
avanzar()

@nivel 1-5
@titulo es: La llave y la puerta
@titulo en: The key and the door
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
@pista es: La llave se junta sola al pasar. La puerta se abre si la tenés.
@logo
avanzar 5
girar derecha
avanzar 3
@lua
avanzar(5)
girar_derecha()
avanzar(3)
@python
avanzar(5)
girar_derecha()
avanzar(3)

@nivel 2-2
@titulo es: La escalera
@titulo en: The staircase
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
@pista es: Mirá un escalón: avanzar, girar a la izquierda, avanzar, girar a la derecha. ¿Cuántos hay?
@logo
repetir 4 [
  avanzar
  girar izquierda
  avanzar
  girar derecha
]
@lua
for i = 1, 4 do
  avanzar()
  girar_izquierda()
  avanzar()
  girar_derecha()
end
@python
for _ in range(4):
    avanzar()
    girar_izquierda()
    avanzar()
    girar_derecha()

@nivel 2-4
@titulo es: La vuelta a la laguna
@titulo en: Around the pond
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
@pista es: Hay que dar la vuelta entera y volver a la meta, que está donde arrancás.
@logo
repetir 4 [ avanzar 3 comer girar derecha ]
@lua
for i = 1, 4 do avanzar(3) comer() girar_derecha() end
@python
for _ in range(4):
    avanzar(3)
    comer()
    girar_derecha()

@-- The pond: the turtle starts on the goal, so "@inicio" places it (x, y
@-- from the top left, counting from 0) and the map keeps the "M".

@nivel 3-2
@titulo es: Dos piedras
@titulo en: Two rocks
@ordenes avanzar girar_derecha girar_izquierda
@meta llegar
@estrellas 13 17
@mapa
###########
##...#...##
#>.#...#.M#
###########
@pista es: Rodear una piedra lleva ocho órdenes. ¿Y si le ponés un nombre?
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
@lua
function rodear()
  girar_izquierda()
  avanzar()
  girar_derecha()
  avanzar(2)
  girar_derecha()
  avanzar()
  girar_izquierda()
end
avanzar()
rodear()
avanzar(2)
rodear()
avanzar()
@python
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
@mapa
##########
#>...L..M#
##########
@pista es: Tu programa se prueba en tres caminos. En cada casilla preguntá si hay lechuga.
@logo
repetir 7 [
  avanzar
  si hay_lechuga [ comer ]
]
@lua
for i = 1, 7 do
  avanzar()
  if hay_lechuga() then comer() end
end
@python
for _ in range(7):
    avanzar()
    if hay_lechuga():
        comer()

@nivel 4-3
@titulo es: Charcos
@titulo en: Puddles
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
@lua
function rodear()
  girar_izquierda()
  avanzar()
  girar_derecha()
  avanzar(2)
  girar_derecha()
  avanzar()
  girar_izquierda()
end
for i = 1, 4 do
  if agua_adelante() then rodear() else avanzar(2) end
end
@python
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

@nivel 5-1
@titulo es: Hasta el fondo
@titulo en: All the way
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
@logo
mientras no en_meta [
  si camino_adelante [ avanzar ] sino [ girar derecha ]
]
@lua
while not en_meta() do
  if camino_adelante() then avanzar() else girar_derecha() end
end
@python
while not en_meta():
    if camino_adelante():
        avanzar()
    else:
        girar_derecha()

@nivel 5-4
@titulo es: El laberinto
@titulo en: The maze
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
@logo
mientras no en_meta [
  si camino_derecha [
    girar derecha
    avanzar
  ] sino [
    si camino_adelante [ avanzar ] sino [ girar izquierda ]
  ]
]
@lua
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
@python
while not en_meta():
    if camino_derecha():
        girar_derecha()
        avanzar()
    elif camino_adelante():
        avanzar()
    else:
        girar_izquierda()

@nivel 6-2
@titulo es: Contá las lechugas
@titulo en: Count the lettuce
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
@pista es: Hace falta una variable que arranque en 0 y sume uno por cada lechuga.
@logo
cuenta = 0
repetir 8 [
  avanzar
  si hay_lechuga [ cuenta = cuenta + 1 ]
]
decir cuenta
@lua
local cuenta = 0
for i = 1, 8 do
  avanzar()
  if hay_lechuga() then cuenta = cuenta + 1 end
end
decir(cuenta)
@python
cuenta = 0
for _ in range(8):
    avanzar()
    if hay_lechuga():
        cuenta += 1
decir(cuenta)

@nivel 7-2
@titulo es: Escalones de todos los tamaños
@titulo en: Steps of every size
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
@lua
function escalon(n)
  avanzar(n)
  girar_izquierda()
  avanzar(n)
  girar_derecha()
end
escalon(1)
escalon(2)
escalon(3)
@python
def escalon(n):
    avanzar(n)
    girar_izquierda()
    avanzar(n)
    girar_derecha()

escalon(1)
escalon(2)
escalon(3)

@nivel 8-1
@titulo es: El cuadrado
@titulo en: The square
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 3 9
@inicio -50 -50 norte
@figura
avanzar 100 girar derecha 90 avanzar 100 girar derecha 90
avanzar 100 girar derecha 90 avanzar 100
@pista es: Cuatro lados iguales, y en cada esquina un giro de 90.
@logo
repetir 4 [ avanzar 100 girar derecha 90 ]
@lua
for i = 1, 4 do avanzar(100) girar_derecha(90) end
@python
for _ in range(4):
    avanzar(100)
    girar_derecha(90)

@nivel 8-3
@titulo es: La estrella
@titulo en: The star
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 3 11
@inicio -75 -50 este
@figura
avanzar 150 girar derecha 144 avanzar 150 girar derecha 144 avanzar 150
girar derecha 144 avanzar 150 girar derecha 144 avanzar 150
@pista es: Cinco puntas. En total la tortuga da dos vueltas enteras: 720 grados.
@logo
repetir 5 [ avanzar 150 girar derecha 144 ]
@lua
for i = 1, 5 do avanzar(150) girar_derecha(144) end
@python
for _ in range(5):
    avanzar(150)
    girar_derecha(144)

@nivel 8-6
@titulo es: La espiral
@titulo en: The spiral
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
@pista es: Cada lado es 10 más largo que el anterior: guardá el largo en una variable.
@logo
lado = 10
repetir 20 [
  avanzar lado
  girar derecha 90
  lado = lado + 10
]
@lua
local lado = 10
for i = 1, 20 do
  avanzar(lado)
  girar_derecha(90)
  lado = lado + 10
end
@python
lado = 10
for _ in range(20):
    avanzar(lado)
    girar_derecha(90)
    lado += 10

@nivel 8-8
@titulo es: La flor
@titulo en: The flower
@ordenes avanzar girar_derecha girar_izquierda bajar_lapiz subir_lapiz
@meta dibujo
@estrellas 7 10
@inicio 0 0 norte
@figura
repetir 12 [ repetir 6 [ avanzar 50 girar derecha 60 ] girar derecha 30 ]
@pista es: Primero enseñale a dibujar un polígono de cualquier cantidad de lados.
@logo
para poligono lados largo
  repetir lados [ avanzar largo girar derecha 360 / lados ]
fin
repetir 12 [ poligono 6 50 girar derecha 30 ]
@lua
function poligono(lados, largo)
  for i = 1, lados do avanzar(largo) girar_derecha(360 / lados) end
end
for i = 1, 12 do poligono(6, 50) girar_derecha(30) end
@python
def poligono(lados, largo):
    for _ in range(lados):
        avanzar(largo)
        girar_derecha(360 / lados)

for _ in range(12):
    poligono(6, 50)
    girar_derecha(30)
