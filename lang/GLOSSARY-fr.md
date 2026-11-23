# Glosario: francés (`fr`)

Para traducir P4OS al francés sin que la misma cosa tenga dos nombres. El
castellano (rioplatense, de vos) es la clave de los catálogos; el inglés
(`lang/en`) es la referencia de sentido y el alemán (`lang/de`) la de
"texto largo que todavía entra". Lo que no está acá se traduce como lo
haría un iPhone en francés, pero tuteando.

> **Sin revisar por hablantes nativos.** Esta traducción la hizo un modelo
> con este glosario y puede tener errores. Quien encuentre uno nos hace un
> favor reportándolo como issue en el GitHub de P4OS.

## Tono

- **Tu**, el tratamiento informal habitual en el software de hobby:
  "Touche Détecter", "Branche le câble", "Choisis un réseau". Nunca "vous".
- **Corto.** El francés se alarga: botones de una palabra ("Enregistrer"
  es largo, "OK"/"Fermer"/"Annuler" no), etiquetas sin artículo, errores
  con "Impossible de…" o directo al grano ("Pas de carte SD"). Si no entra,
  se resume, no se abrevia con puntos raros.
- **Mayúsculas como en castellano:** sólo la primera palabra ("Portail web",
  "Date et heure"). Lo que el original escribe en MAYÚSCULAS va en
  mayúsculas, con sus acentos (É, À).
- **Puntuación:** espacio de no separación antes de `:` `;` `?` `!` y dentro
  de « » (en los catálogos es el carácter U+00A0 literal). Comillas « »
  donde el castellano usa comillas.
- Números: lo que pone el código (`%.1f`, `%d`) no se toca; los escritos en
  el texto, con coma decimal, salvo valores técnicos (IPs, versiones, "3V3").

## Términos

| castellano | inglés | **francés** | nota |
|---|---|---|---|
| placa | board | **carte** | "la carte"; el aparato |
| tarjeta (microSD) | card | **carte SD** | siempre con "SD" (o "microSD"), para no confundirla con la placa |
| portal (web) | (web) portal | **portail (web)** | "le portail" |
| Tienda | Store | **Boutique** | nombre de app, con mayúscula |
| Ajustes | Settings | **Réglages** | como iOS |
| header / conector de 40 pines | header | **connecteur** | "le connecteur 40 broches" |
| pin | pin | **broche** | "broche 7", GPIO igual |
| inicio / pantalla de inicio | home / home screen | **Accueil / écran d'accueil** | |
| pantalla de bloqueo | lock screen | **écran verrouillé** | el rótulo de Réglages es "Verrouillage": acostada no entra más |
| pantalla | screen / display | **écran** | "Écran" en Réglages |
| tocar / tocá | tap | **toucher / touche** | "Touche Enregistrer" |
| arrastrar | drag | **faire glisser / glisse** | |
| mantener apretado | hold | **maintenir appuyé** | |
| computadora | computer | **ordinateur** | |
| modo Disco | Disk mode | **mode Disque** | |
| Wi-Fi | Wi-Fi | **Wi-Fi** | |
| red / redes | network(s) | **réseau / réseaux** | |
| emparejar | pair | **jumeler** | "jumelé" |
| notificación | notification | **notification** | |
| actualizar / actualización | update | **mettre à jour / mise à jour** | botón: "Mettre à jour" |
| Obtener (Tienda) | Get | **Obtenir** | |
| instalar / desinstalar | install / uninstall | **installer / désinstaller** | |
| respaldo (copia de seguridad) | backup | **sauvegarde** | |
| Desarrollador | Developer | **Développeur** | |
| archivo / carpeta | file / folder | **fichier / dossier** | |
| captura (de pantalla) | screenshot | **capture d'écran** | |
| contraseña | password | **mot de passe** | |
| reiniciar | restart | **redémarrer** | |
| registro (log) | log | **journal** | |
| app / apps | app / apps | **app / apps** | "l'app" (femenino: "une app") |
| teclado | keyboard | **clavier** | |
| mando (gamepad) | gamepad | **manette** | |
| auriculares | headphones | **écouteurs** | |
| parlante | speaker | **haut-parleur** | |
| grabar (a flash) | write / flash | **écrire / flasher** | |
| grabar (audio/video) | record | **enregistrer** | |

Abreviaturas de días (contexto `dia`): DIM LUN MAR MER JEU VEN SAM.
Iniciales (contexto `lun`…`dom`): L M M J V S D.
Meses (contexto `mes`): JAN FÉV MAR AVR MAI JUN JUL AOÛ SEP OCT NOV DÉC.

## Nombres de las apps (inicio, `_sistema.lang`)

Los que el inglés deja sin traducir (nombres propios, marcas, siglas)
quedan iguales: Cartero, Chatarra, Claudito, Claude, Claude Jump, Mila,
Pato goma, Truco, ARKANOS, 2043, Doom, Duke Nukem 3D, Flappy, Monster Hop,
Neon Snakes, Pinball 3D, Pixel Art, Pomodoro, Blackjack, Golf, Sudoku,
Simon, Turbo, Datalog, Macro pad, Blit test, tap, Home Assistant y las
siglas (BLE, CAN, CAS, DMX, EEPROM, MQTT, PWM, RF, SSH, VNC, Modbus, Python,
Bus).

| castellano | inglés | **francés** |
|---|---|---|
| Música | Music | Musique |
| Ajustes | Settings | Réglages |
| Notas | Notes | Notes |
| Archivos | Files | Fichiers |
| Infrarrojo | Infrared | Infrarouge |
| Reloj | Clock | Horloge |
| Alarmas | Alarms | Alarmes |
| Temporizador | Timer | Minuteur |
| Fotos | Photos | Photos |
| Terminal | Terminal | Terminal |
| Monitor | Monitor | Moniteur |
| Programador | Flasher | Programmateur |
| Vida | Life | Vie |
| Hola | Hello | Bonjour |
| Red | Network | Réseau |
| Mapas | Maps | Plans |
| Tienda | Store | Boutique |
| Banco | Bench | Banc |
| Calculadora | Calculator | Calculette |
| Calendario | Calendar | Calendrier |
| Conversor | Converter | Convertisseur |
| Electrónica | Electronics | Électronique |
| Tiras LED | LED Strips | Rubans LED |
| Módulos | Modules | Modules |
| Control PC | PC Control | Contrôle PC |
| Cronometro | Stopwatch | Chronomètre |
| Reloj mundial | World Clock | Horloge mondiale |
| Afinador | Tuner | Accordeur |
| Atasco | Traffic Jam | Bouchon |
| Bastión | Bastion | Bastion |
| Bolsa | Stocks | Bourse |
| Burbujas | Bubbles | Bulles |
| Buscaminas | Minesweeper | Démineur |
| Cámaras | Cameras | Caméras |
| Caños | Pipes | Tuyaux |
| Clima | Weather | Météo |
| Cotizaciones | Currencies | Devises |
| Dados | Dice | Dés |
| Dibujo | Drawing | Dessin |
| Emulador | Emulator | Émulateur |
| Equipos | Equipment | Appareils |
| Estudio | Studio | Studio |
| Gemas | Gems | Gemmes |
| Hockey de mesa | Air Hockey | Air hockey |
| Impresoras 3D | 3D Printers | Imprimantes 3D |
| Lógica | Logic | Logique |
| Luces | Lights | Lumières |
| Juegos de mesa | Board games | Jeux de société |
| Metegol | Table Football | Baby-foot |
| Oficina | Office | Bureau |
| Radio | Radio | Radio |
| Grabadora | Recorder | Enregistreur |
| Solitario | Solitaire | Solitaire |
| Topos | Whack-a-Mole | Taupes |
| Traductor | Translator | Traducteur |
| Video | Video | Vidéo |
| Visor 3D | 3D Viewer | Visionneuse 3D |
| Vuelos | Flights | Vols |

Metegol se traduce porque el inglés también lo traduce (Table Football):
la regla es la del inglés.

## Caracteres

- Sólo Latin-1 (à â ç é è ê ë î ï ô ù û ü ÿ « »), más los símbolos que ya
  traiga el original (… • → ° µ Ω, emojis, glifos del sistema).
- **Sin œ:** no está en las fuentes mono ni en las Inter grandes (48 y
  64). Se escribe "oe" ("noeud", "oeil", "manoeuvre") hasta que el
  firmware la sume (pedido anotado).
- Apóstrofo recto `'`, no el tipográfico `’`, que no está en las mono.
- Demo 2043, ARKANOS, Claudito y Chatarra dibujan con una fuente propia
  sólo ASCII: en sus catálogos, **sin acentos ni cedilla** ("Reglages",
  "Ecran") y sin espacio antes de `:` `?` `!`.
