# P4OS — Header de expansión y módulos

La P4 no es un reloj: está en el banco de trabajo, enchufada, y el conector de 40
pines de atrás es lo que la convierte en herramienta. La idea es que el
hardware extra (RS485, sensores I2C, LoRa, CAN, relés…) se conecte al header y
que **apps que se instalan en la SD** lo usen sin reflashear el firmware.

## El header (J3, 2,54 mm)

⚠️ **La numeración es la de una Raspberry Pi con las columnas cruzadas**: pin 1 =
5 V y pin 2 = 3V3. Mirando la placa, los pines impares (1 = 5 V arriba)
están en la columna de la **derecha** y los pares (2 = 3V3) en la de la
izquierda, como el dibujo de Waveshare; la tabla y todas las vistas del
conector (Ajustes → Expansión, Bus, Módulos, el portal) lo muestran así
desde el 2026-09-30. Un HAT de Raspberry no se enchufa directo (Waveshare dice que
hace falta adaptador).

| Pin | Señal | Estado | Pin | Señal | Estado |
|---|---|---|---|---|---|
| 2 | 3V3 | ⚡ buck de 3 A, se apaga con POWER | 1 | 5 V | ⚡ vivo aun apagada (esquemático) |
| 4 | GPIO7 SDA | 🟡 I2C de la placa | 3 | 5 V | ⚡ |
| 6 | GPIO8 SCL | 🟡 I2C de la placa | 5 | GND |  |
| 8 | GPIO2 | 🟢 (🟡 si R108 lleva la INT del táctil) | 7 | GPIO37 | 🔴 UART0 TX → CH343 (consola) |
| 10 | GND |  | 9 | GPIO38 | 🔴 UART0 RX ← CH343 (consola) |
| 12 | GPIO3 | 🟢 (JTAG MTCK) | 11 | GPIO5 | 🟢 (JTAG MTDO) |
| 14 | GPIO4 | 🟢 (JTAG MTDI) | 13 | GND |  |
| 16 | GPIO28 | 🟢 | 15 | GPIO21 | 🟢 ADC1 canal 5 |
| 18 | 3V3 | ⚡ | 17 | GPIO22 | 🟢 ADC1 canal 6 |
| 20 | GPIO29 | 🟢 | 19 | GND |  |
| 22 | GPIO30 | 🟢 | 21 | GPIO24 | 🟡 USB D− del pendrive en modo host (`docs/USB.md`); si no, libre |
| 24 | GPIO31 | 🟢 | 23 | GPIO25 | 🟡 USB D+ del pendrive en modo host |
| 26 | GND |  | 25 | USBD_N | 🔴 USB HS (en paralelo con H2), no es GPIO |
| 28 | GPIO34 | 🟡 strapping (JTAG), usable tras el arranque | 27 | USBD_P | 🔴 USB HS, no es GPIO |
| 30 | GPIO35 | 🔴 BOOT (strapping, pull-up 4,7K) | 29 | GND |  |
| 32 | GPIO49 | 🟢 ADC2 canal 0 | 31 | GPIO32 | 🟢 |
| 34 | GPIO50 | 🟢 ADC2 canal 1 | 33 | GND |  |
| 36 | GPIO51 | 🟢 ADC2 canal 2, comparador | 35 | GPIO46 | 🟡 dominio VO4: **medir tensión** |
| 38 | GPIO52 | 🟢 ADC2 canal 3, comparador | 37 | GPIO47 | 🟡 dominio VO4 |
| 40 | GND |  | 39 | GPIO48 | 🟡 dominio VO4 |

🟢 libre · 🟡 libre con condiciones · 🔴 ocupado · ⚡ alimentación

**Libres de verdad: 14** (3, 4, 5, 21, 22, 28, 29, 30, 31, 32, 49, 50, 51, 52),
más 2, 34 y 46-48 con condiciones, y 24/25 si se renuncia al USB-JTAG.

### Sobre GPIO37/38 para RS485

No conviene: son el UART0 que va al CH343P del puerto H1. Por ahí se graba la
placa y sale la consola. El TX del CH343 maneja GPIO38 todo el tiempo, así que
un transceptor RS485 ahí se pelearía con él. El P4 tiene **matriz de GPIO y 5
UARTs**: cualquier UART sale por cualquier pin libre. Por eso el RS485 va en el
perfil de abajo, en 29/30/31.

### Sobre el I2C

El bus de la placa (GPIO7/8, 400 kHz, pull-ups de 2,2K ya puestas) sale al
header y **se puede compartir**, pero ya tiene ocupadas 0x14/0x5D (táctil), 0x18
(ES8311), 0x40 (ES7210) y 0x36 (cámara, si hay). **0x40 choca con sensores muy
comunes**: HTU21D/SHT21, Si7021 e INA219. Por eso el perfil trae un **segundo
bus I2C sólo para lo externo** (el P4 tiene dos I2C HP más uno LP). Además, un
sensor colgado del bus de la placa que se trabe deja sin táctil a toda la placa.

## Perfil de pines por defecto (borrador, se congela en la Fase 2)

Pensado para que cada "conector lógico" caiga en pines vecinos, con GND y 3V3
cerca, y se pueda armar un cable o una placa hija simple.

| Puerto lógico | Señales → GPIO (pin físico) | Para qué |
|---|---|---|
| `i2c.ext` | SDA 21 (15), SCL 22 (17), 3V3 (18), GND (19) | Sensores I2C externos, bus propio (pull-ups externas) |
| `i2c.board` | SDA 7 (4), SCL 8 (6) | Bus de la placa, sólo si hace falta (cuidado con 0x40) |
| `uart.a` | TX 30 (22), RX 31 (24), DE/RTS 29 (20), GND (26) | RS485 / Modbus RTU (el UART del P4 maneja DE solo en modo RS485 half-duplex) |
| `uart.b` + control | TX 3 (12), RX 4 (14), EN 5 (11), BOOT 2 (8), GND (10/13) | Terminal serie y **programador de otras placas** (reset y modo boot del objetivo) |
| `spi.a` | SCK 52 (38), MOSI 50 (34), MISO 51 (36), CS 49 (32) | LoRa, pantallas, ADCs externos |
| `spi.a` auxiliares | 46 (35), 47 (37), 48 (39) | DIO1 / BUSY / RST de un SX1262 o SX127x (**si la tensión de VO4 da 3,3 V**) |
| `gpio` sueltos | 28 (16), 32 (31), 34 (28) | 1-Wire, WS2812, relés, entradas, CAN (TWAI TX/RX + transceptor) |
| `adc` | 21, 22 (si no se usa `i2c.ext`), 49-52 (si no se usa `spi.a`) | Voltímetro / registrador |

Nada de esto es fijo: el perfil es un archivo en la SD y se edita desde Ajustes o
desde el portal. El SO sólo se asegura de que dos cosas no se pisen.

## Arquitectura de módulos

Tres capas, de abajo hacia arriba.

### 1. Árbitro de pines (`aos_pins`, en firmware)

- Conoce el mapa de la placa: qué GPIO está en el header, en qué pin físico,
  cuáles son internos (prohibidos), de strapping o de otro dominio de tensión.
- Cada uso **reserva** pines con un dueño (`"sistema"`, `"módulo rs485"`,
  `"app com.x.lora"`) y se niega si ya están tomados, con un mensaje claro
  ("GPIO30 lo tiene el módulo rs485").
- Se liberan solos cuando la app que los tomó se cierra.
- La tabla viva se ve en Ajustes → Expansión y en `/expansion` del portal
  (el header dibujado, coloreado por dueño).

### 2. Servicios de bus (en firmware, API `aos_io_*` estable para las `.so`)

Las apps dinámicas no llaman a los drivers de IDF directamente (romperían con cada
versión); usan una API chica y estable, que también existe en el simulador:

- `aos_io_gpio_*` (modo, leer, escribir, interrupción con callback en el worker)
- `aos_io_i2c_*` (abrir un bus lógico, transferir, escanear)
- `aos_io_spi_*` (dispositivo sobre un bus lógico, transferencia con DMA) — **hecho el 2026-09-30**: API en `aos_io.h`, GPSPI2/3 en la placa, chips emulados en el simulador y la pestaña SPI de Bus; detalles en [MODULES.md](../MODULES.md#spi)
- `aos_io_uart_*` (abrir por puerto lógico, baudios, modo RS485, leer/escribir sin bloquear)
- `aos_io_pwm_*`, `aos_io_adc_*`, más adelante `aos_io_twai_*` (CAN) y `aos_io_rmt_*` (WS2812, IR)

Se abre **por nombre de puerto lógico** (`aos_io_uart_open("uart.a")`), no por
número de pin. Así la app no sabe ni le importa dónde está conectado el módulo.

### 3. Módulos (archivo `modules.txt` en la SD, mismo estilo que `menu.txt`)

```
# puerto lógico = qué hay conectado
port uart.a tx=30 rx=31 de=29
port uart.b tx=3 rx=4
port i2c.ext sda=21 scl=22 freq=100000
port spi.a sck=52 mosi=50 miso=51 cs=49 freq=1000000

module rs485    uart.a baud=9600 parity=N      # Modbus RTU al Riden / al gateway
module target   uart.b en=5 boot=2 baud=115200 # placa a programar
module bme280   i2c.ext addr=0x76
module sx1262   spi.a cs=49 dio1=46 busy=47 rst=48 freq=915000000
```

- Los módulos de uso común traen **driver en firmware** (RS485, sensores I2C
  conocidos, DS18B20, WS2812, relés): cualquier app los lee sin traer código.
- Los raros los trae **la propia app** (un driver de SX1262 adentro de la `.so`
  de LoRa) usando `aos_io_spi_*`.
- **Autodetección I2C**: al abrir `i2c.ext` se escanea y las direcciones
  conocidas se proponen ("apareció algo en 0x76, ¿es un BME280?").
- Una app declara lo que necesita en su descriptor
  (`requires: "uart.rs485"`), y el lanzador avisa si no está configurado en vez
  de abrirla y fallar.

### En el simulador

- `uart.*` se puede mapear a un puerto serie real de la Mac
  (`P4_SIM_UART_A=/dev/cu.usbserial-110`). Así la Terminal serie, Modbus y el
  programador se desarrollan con los adaptadores USB-serie que ya tenés, antes
  de que llegue la placa.
- `i2c.*` tiene sensores falsos configurables (un BME280 que devuelve una curva).
- `gpio` muestra un panel con el header dibujado para ver qué pines cambian.

## Módulos candidatos (para ordenar después)

| Módulo | Hardware | Apps que lo aprovechan |
|---|---|---|
| RS485 | MAX485 / placa RS485 automática en `uart.a` | Modbus RTU maestro/esclavo, Riden RD6012, gateway DOMCOM, sniffer de bus |
| Sensores I2C | BME280/680, SHT3x, SCD40, INA219/226, BH1750, ADS1115 | Panel ambiental, registrador, medidor de consumo, publicación a HA |
| Placa a programar | cable de 4-6 hilos a `uart.b` | Terminal serie, programador ESP32/ESP8266/STM32, puente RFC2217 |
| LoRa | SX1262 / SX1276 / RFM95 en `spi.a` | Chat LoRa, pasarela Meshtastic/LoRaWAN, telemetría del campo |
| CAN | transceptor SN65HVD230 + TWAI | Sniffer CAN, OBD-II |
| 1-Wire | DS18B20 | Temperaturas (reusar el driver del gateway) |
| WS2812 | tira LED por RMT | Luz de estado, iluminación del banco |
| Relés / optos | GPIO sueltos | Control de banco (encender la fuente, reset de placas) |
| Analizador lógico | 8 GPIO por PARLIO/RMT | Captura y decodificación UART/I2C/SPI (ambicioso, Fase 8) |
