# Glosario: portugués de Brasil (`pt`)

Para traducir P4OS al portugués de Brasil sin que la misma cosa tenga dos
nombres. El castellano (rioplatense, de vos) es la clave de los catálogos;
el inglés (`lang/en`) es la referencia de sentido y el alemán (`lang/de`) la
de "texto largo que todavía entra". Lo que no está acá se traduce como lo
haría un iPhone en pt-BR.

> **Sin revisar por hablantes nativos.** Esta traducción la hizo un modelo
> con este glosario y puede tener errores. Quien encuentre uno nos hace un
> favor reportándolo como issue en el GitHub de P4OS.

## Tono

- **Você**, con el imperativo de tercera persona que usa iOS:
  "Toque em Detectar", "Conecte o cabo", "Escolha uma rede". Nunca "tu".
- **Corto.** Botones de una palabra ("Salvar", "Cancelar"), etiquetas sin
  artículo, errores con "Não foi possível…" o directo al grano
  ("Sem cartão"). Si el castellano corta con dos puntos o con una coma, el
  portugués también.
- **Mayúsculas como en castellano:** sólo la primera palabra y los nombres
  propios ("Portal web", "Data e hora"). Lo que el original escribe en
  MAYÚSCULAS (títulos de sección: "DISPOSITIVOS") va en mayúsculas.
- **Ortografía del Acordo Ortográfico de 1990** (pt-BR): "eletrônica",
  "ideia", "conexão", "atualização".
- Números: lo que pone el código (`%.1f`, `%d`) no se toca; los que están
  escritos en el texto siguen la costumbre brasileña (coma decimal), salvo
  valores técnicos que el usuario copia (IPs, versiones, "3V3").

## Términos

| castellano | inglés | **portugués** | nota |
|---|---|---|---|
| placa | board | **placa** | el aparato; "a placa" |
| tarjeta (microSD) | card | **cartão** | "o cartão", "cartão microSD"; nunca "placa" |
| portal (web) | (web) portal | **portal (web)** | "o portal" |
| Tienda | Store | **Loja** | nombre de app, con mayúscula |
| Ajustes | Settings | **Ajustes** | como iOS pt-BR |
| header / conector de 40 pines | header | **header** | "o header de 40 pinos" |
| pin | pin | **pino** | "pino 7", "GPIO" igual |
| inicio / pantalla de inicio | home / home screen | **Início / tela de início** | |
| pantalla de bloqueo | lock screen | **tela de bloqueio** | el rótulo de Ajustes es "Bloqueio": acostada no entra más |
| pantalla | screen / display | **tela** | "Tela" en Ajustes |
| tocar / tocá | tap | **tocar / toque em** | "Toque em Salvar" |
| arrastrar | drag | **arrastar** | |
| mantener apretado | hold | **manter pressionado** | |
| computadora | computer | **computador** | |
| modo Disco | Disk mode | **modo Disco** | |
| Wi-Fi | Wi-Fi | **Wi-Fi** | |
| red / redes | network(s) | **rede / redes** | |
| emparejar | pair | **parear** | "pareado" |
| notificación | notification | **notificação** | |
| actualizar / actualización | update | **atualizar / atualização** | |
| Obtener (Tienda) | Get | **Obter** | |
| instalar / desinstalar | install / uninstall | **instalar / desinstalar** | |
| respaldo (copia de seguridad) | backup | **backup** | "Backup" en Ajustes |
| Desarrollador | Developer | **Desenvolvedor** | |
| archivo / carpeta | file / folder | **arquivo / pasta** | |
| captura (de pantalla) | screenshot | **captura de tela** | |
| contraseña | password | **senha** | |
| token | token | **token** | |
| reiniciar | restart | **reiniciar** | |
| registro (log) | log | **registro** | |
| app / apps | app / apps | **app / apps** | "o app" |
| firmware | firmware | **firmware** | |
| teclado | keyboard | **teclado** | |
| mando (gamepad) | gamepad | **controle** | |
| auriculares | headphones | **fones de ouvido** | |
| parlante | speaker | **alto-falante** | |
| grabar (a flash) | write / flash | **gravar** | |
| grabar (audio/video) | record | **gravar** | |

Abreviaturas de días (contexto `dia`): DOM SEG TER QUA QUI SEX SÁB.
Iniciales (contexto `lun`…`dom`): S T Q Q S S D (lunes a domingo).
Meses (contexto `mes`): JAN FEV MAR ABR MAI JUN JUL AGO SET OUT NOV DEZ.

## Nombres de las apps (inicio, `_sistema.lang`)

Los que el inglés deja sin traducir (nombres propios, marcas, siglas)
quedan iguales: Cartero, Chatarra, Claudito, Claude, Claude Jump, Mila,
Pato goma, Truco, ARKANOS, 2043, Doom, Duke Nukem 3D, Flappy, Monster Hop,
Neon Snakes, Pinball 3D, Pixel Art, Pomodoro, Blackjack, Golf, Sudoku,
Simon, Turbo, Datalog, Macro pad, Blit test, tap, Home Assistant y las
siglas (BLE, CAN, CAS, DMX, EEPROM, MQTT, PWM, RF, SSH, VNC, Modbus, Python,
Bus).

| castellano | inglés | **portugués** |
|---|---|---|
| Música | Music | Música |
| Ajustes | Settings | Ajustes |
| Notas | Notes | Notas |
| Archivos | Files | Arquivos |
| Infrarrojo | Infrared | Infravermelho |
| Reloj | Clock | Relógio |
| Alarmas | Alarms | Alarmes |
| Temporizador | Timer | Timer |
| Fotos | Photos | Fotos |
| Terminal | Terminal | Terminal |
| Monitor | Monitor | Monitor |
| Programador | Flasher | Programador |
| Vida | Life | Vida |
| Hola | Hello | Olá |
| Red | Network | Rede |
| Mapas | Maps | Mapas |
| Tienda | Store | Loja |
| Banco | Bench | Bancada |
| Calculadora | Calculator | Calculadora |
| Calendario | Calendar | Calendário |
| Conversor | Converter | Conversor |
| Electrónica | Electronics | Eletrônica |
| Tiras LED | LED Strips | Fitas LED |
| Módulos | Modules | Módulos |
| Control PC | PC Control | Controle PC |
| Cronometro | Stopwatch | Cronômetro |
| Reloj mundial | World Clock | Relógio mundial |
| Afinador | Tuner | Afinador |
| Atasco | Traffic Jam | Trânsito |
| Bastión | Bastion | Bastião |
| Bolsa | Stocks | Bolsa |
| Burbujas | Bubbles | Bolhas |
| Buscaminas | Minesweeper | Campo minado |
| Cámaras | Cameras | Câmeras |
| Caños | Pipes | Canos |
| Clima | Weather | Clima |
| Cotizaciones | Currencies | Cotações |
| Dados | Dice | Dados |
| Dibujo | Drawing | Desenho |
| Emulador | Emulator | Emulador |
| Equipos | Equipment | Equipamentos |
| Estudio | Studio | Estúdio |
| Gemas | Gems | Gemas |
| Hockey de mesa | Air Hockey | Air hockey |
| Impresoras 3D | 3D Printers | Impressoras 3D |
| Lógica | Logic | Lógica |
| Luces | Lights | Luzes |
| Juegos de mesa | Board games | Jogos de mesa |
| Metegol | Table Football | Pebolim |
| Oficina | Office | Escritório |
| Radio | Radio | Rádio |
| Grabadora | Recorder | Gravador |
| Solitario | Solitaire | Paciência |
| Topos | Whack-a-Mole | Toupeiras |
| Traductor | Translator | Tradutor |
| Video | Video | Vídeo |
| Visor 3D | 3D Viewer | Visor 3D |
| Vuelos | Flights | Voos |

Metegol se traduce porque el inglés también lo traduce (Table Football):
la regla es la del inglés.

## Caracteres

- Sólo Latin-1 (á à â ã ç é ê í ó ô õ ú ü), más los símbolos que ya traiga
  el original (… • → ° µ Ω, emojis, glifos del sistema).
- Comillas rectas `"` y apóstrofo recto `'`: las tipográficas (“ ” ‘ ’) no
  están en las fuentes mono.
- Demo 2043, ARKANOS, Claudito y Chatarra dibujan con una fuente propia
  sólo ASCII: en sus catálogos, **sin acentos ni cedilla** ("Configuracoes",
  "Nao") y en el largo del original.
