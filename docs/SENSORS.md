# Sensors

P4OS reads sensors by itself: a service in the firmware
(`components/aos_io/aos_sensors.c`) goes round once a second, reads every
sensor `modules.txt` declares and every one it recognises on its own, and
keeps the latest values and five minutes of history. Módulos, the portal's
Expansión page, Datalog and any app see the same readings
(`aos_sensors.h`). Nobody else has to talk to the bus.

Since 0.20 the service knows 112 chips: the I2C breakouts the usual shops
sell (Adafruit, SparkFun, Seeed, Waveshare, Pimoroni, DFRobot, M5Stack, the
GY-xxx boards), plus SPI converters, 1-Wire, sensors on plain GPIOs and
serial ones. The idea is that a known module plugged into P4OS just works.

This page is the catalogue and what each chip needs. The header, its ports,
the wiring and `modules.txt` itself are in [MODULES.md](MODULES.md).

## Connecting one

| Bus | Where | What P4OS does |
|---|---|---|
| I2C | `i2c.ext`: SDA GPIO21 (pin 15), SCL GPIO22 (pin 17), 3V3 (pin 18), GND | Recognises it by itself in a few seconds, unless the table says "declared" |
| SPI | `spi.a`, each chip with its own CS | Declared: `module max31855 spi.a cs=48` |
| 1-Wire | a GPIO port: `port gpio.ow pin=34` | Declared bus; its sensors are found by a search |
| GPIO | a GPIO port, and `pin2=` for a second pin | Declared: `port gpio.dht pin=2` and `module dht22 gpio.dht` |
| UART | `uart.a` (TX GPIO30, RX GPIO31) | Declared: `module mhz19 uart.a`; the sensor sets the speed |

The easy way to declare one is **Módulos > Puertos > Agregar** (or the
portal's Expansión page): pick what it measures, the chip, and the port,
pins or address, and the form writes the line. The module id is what goes
in the line; any of its other names (the "also known as" column) works too,
and so does a name of your own with `chip=`:
`module heladera gpio.dht chip=dht22`.

Each chip is read at its own speed, whatever the port's `freq=`: the
100 kHz parts (PCF8591, PCF8574, AM2320, and those that stretch the clock,
like the SCD30 or the SEN5x) at 100 kHz, the rest at the port's. The
PCF8591 is the one that shows it: it converts on SCL's clock, and at
400 kHz its inputs read anything.

Everything on the header is 3.3 V. A 5 V module's outputs (an HC-SR04's
ECHO, a 5 V PIR) need a divider or a level shifter before the pin.

## What autodetect never does

Recognising a chip is reading it. The service follows a few rules so that
probing a bus never changes what is on it (`components/aos_io/sensors/sens_int.h`):

- **It only reads.** It writes a register pointer and reads, or sends the
  documented read-only command of that chip family (Sensirion's "who are
  you", an SMBus word). It never sends a two-byte write to an unknown chip:
  that would set an MCP4725's output, reconfigure an MCP3421, or burn an
  AS5600's one-time memory.
- **A chip that cannot be told apart safely is declared, never probed:**
  PCF8574, PCF8591, LM75, MCP342x, AS5600, TF-Luna, RCWL-9620. Their
  table row says "declared".
- **A sensor that is reading is never asked again.** An SCD4x stops
  measuring when it is asked who it is. One that is failing (out of range,
  a bad CRC) is first asked by its own driver, so other drivers' probes do
  not poke a chip that is already set up.
- **Some addresses are not probed at all:** 0x70 for the QMP6988 (a
  TCA9548A mux there takes any byte as its channel mask), 0x38/0x39 for
  the Seesaw (a PCF8574A's outputs), 0x61/0x62 for the MCP9600 (the
  SCD30's and SCD4x's).
- **Where several chips share an address,** the one that proves itself
  with a plain register read is tried first, then those that need a
  command, then the weak signatures. 0x29 holds seven (VL53L1X, VL6180X,
  VL53L0X, BNO055, TCS34725, TSL2591, LTR-329); 0x40 seven (INA219/226/228/
  260/3221, HDC1080, HDC2080, SHT2x, HM3301); 0x76/0x77 eight barometers.
- **What answers but proves nothing becomes a candidate,** a question in
  Módulos ("¿Es un BH1750?") with a button that declares it.

## The catalogue

"Found": **auto** is recognised by itself; **declared** goes in
`modules.txt`; **candidate** is asked about. The values are the keys an app
asks for (`aos_sensor_value("bme280", "press", &v)`); in parentheses, the
ones only some models or settings give.

¹ Recognised when its power-on values are there; otherwise a candidate.
² Only recognised while it is already converting on its own; usually
declared (an ADS1115's driver reads it right anyway).
³ An AM2320 sleeps and NACKs the first call: the service calls twice.

### Temperature and humidity (Temperatura y humedad)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| AHT20 | `aht20` | aht10, aht21, aht2x, aht25, aht30, dht20, am2301b, am2315c | I2C 0x38 | auto | `temp` `hum` |
| AM2320 | `am2320` |  | I2C 0x5C | auto ³ | `temp` `hum` |
| DHT11 | `dht11` | dht12 | GPIO | declared | `temp` `hum` |
| DHT22 | `dht22` | am2302, am2303, dht21, am2301 | GPIO | declared | `temp` `hum` |
| DS18B20 | `ds18b20` | ds18s20, ds1822, max31850, onewire | 1-Wire | bus search | `temp` |
| DS3231 | `ds3231` | zs042 | I2C 0x68 | auto | `temp` |
| HDC1080 | `hdc1080` | hdc1000, hdc1008, hdc1050 | I2C 0x40, 0x41, 0x42, 0x43 | auto | `temp` `hum` |
| HDC2080 | `hdc2080` | hdc2010 | I2C 0x40, 0x41 | auto | `temp` `hum` |
| HDC302x | `hdc302x` | hdc3020, hdc3021, hdc3022 | I2C 0x44, 0x45, 0x46, 0x47 | auto | `temp` `hum` |
| LM75 | `lm75` | lm75a, lm75b, tmp75, pct2075, ds75 | I2C 0x48, 0x49, 0x4A, 0x4B | declared | `temp` |
| MAX31855 | `max31855` | max31855k, max31855j, max31855t | SPI | declared | `temp` `cj` |
| MAX31856 | `max31856` |  | SPI | declared | `temp` `cj` |
| MAX31865 | `max31865` | pt100, pt1000 | SPI | declared | `temp` `rtd` |
| MAX6675 | `max6675` |  | SPI | declared | `temp` |
| MCP9808 | `mcp9808` |  | I2C 0x18, 0x19, 0x1A, 0x1B | auto | `temp` |
| MLX90614 | `mlx90614` | gy906 | I2C 0x5A | auto | `obj` `amb` |
| SHT2x | `sht2x` | sht20, sht21, sht25, htu21d, htu20d, htu21, si7021, si7020, si7013, gy21 | I2C 0x40 | auto | `temp` `hum` |
| SHT3x | `sht3x` | sht30, sht31, sht35, sht85, gxht30 | I2C 0x44, 0x45 | auto | `temp` `hum` |
| SHT4x | `sht4x` | sht40, sht41, sht43, sht45 | I2C 0x44, 0x45 | auto | `temp` `hum` |
| SHTC3 | `shtc3` | shtc1 | I2C 0x70 | auto | `temp` `hum` |
| STS3x | `sts3x` | sts30, sts31, sts35 | I2C 0x4A, 0x4B | auto | `temp` |
| TMP102 | `tmp102` | tmp112 | I2C 0x48, 0x49, 0x4A, 0x4B | auto ¹ | `temp` |
| TMP117 | `tmp117` | tmp116, tmp119 | I2C 0x48, 0x49, 0x4A, 0x4B | auto | `temp` |

### Pressure and weather (Presión y clima)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| Anemómetro | `wind` | anemometer | GPIO | declared | `speed` |
| BME280 | `bme280` |  | I2C 0x76, 0x77 | auto | `temp` `press` `hum` |
| BME680 | `bme680` | bme688 | I2C 0x77, 0x76 | auto | `temp` `hum` `press` `gas` |
| BMP180 | `bmp180` | bmp085 | I2C 0x77 | auto | `temp` `press` |
| BMP280 | `bmp280` |  | I2C 0x76, 0x77 | auto | `temp` `press` |
| BMP388 | `bmp3xx` | bmp388, bmp390 | I2C 0x77, 0x76 | auto | `temp` `press` |
| BMP581 | `bmp5xx` | bmp581, bmp585 | I2C 0x47, 0x46 | auto | `temp` `press` |
| DPS310 | `dps310` | dps368, spl06, spl06_001, hp303b | I2C 0x77, 0x76 | auto | `temp` `press` |
| Lluvia | `rain` | rain_gauge | GPIO | declared | `rain` `rainrate` |
| LPS22HB | `lps22` | lps22hb, lps22hh, lps22df, lps25, lps25hb, lps33, lps33hw, lps35, lps35hw | I2C 0x5D, 0x5C | auto | `temp` `press` |
| MS5611 | `ms5611` | ms5607, gy63 | I2C 0x77, 0x76 | auto | `temp` `press` |
| QMP6988 | `qmp6988` |  | I2C 0x70, 0x56 | auto | `temp` `press` |

### Air quality (Calidad del aire)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| CCS811 | `ccs811` |  | I2C 0x5A, 0x5B | auto | `eco2` `tvoc` |
| ENS160 | `ens160` | ens161 | I2C 0x53, 0x52 | auto | `aqi` `eco2` `tvoc` |
| HM3301 | `hm3301` |  | I2C 0x40 | auto | `pm1` `pm25` `pm10` |
| MH-Z19 | `mhz19` | mhz19b, mhz19c, mhz19e | UART | declared | `co2` `temp` |
| PMS5003 | `pms5003` | pms7003, pmsa003, pms3003 | UART | declared | `pm1` `pm25` `pm10` |
| PMSA003I | `pmsa003i` |  | I2C 0x12 | auto | `pm1` `pm25` `pm10` |
| SCD30 | `scd30` |  | I2C 0x61 | auto | `co2` `temp` `hum` |
| SCD4x | `scd4x` | scd40, scd41, scd43 | I2C 0x62 | auto | `co2` `temp` `hum` |
| SDS011 | `sds011` |  | UART | declared | `pm25` `pm10` |
| SEN5x | `sen5x` | sen50, sen54, sen55 | I2C 0x69 | auto | `pm1` `pm25` `pm4` `pm10` (`temp` `hum` `voc` `nox`) |
| Senseair S8 | `senseair_s8` | s8, senseair | UART | declared | `co2` |
| SGP30 | `sgp30` |  | I2C 0x58 | auto | `eco2` `tvoc` (`h2` `ethanol`) |
| SGP4x | `sgp4x` | sgp40, sgp41 | I2C 0x59 | auto | `voc` (`nox`) (`vocraw` `noxraw`) |
| SPS30 | `sps30` |  | I2C 0x69 | auto | `pm1` `pm25` `pm4` `pm10` |

### Light and colour (Luz y color)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| APDS-9960 | `apds9960` | gy9960 | I2C 0x39 | auto | `prox` `clear` `r` `g` `b` |
| BH1750 | `bh1750` | gy302, gy30 | I2C 0x23, 0x5C | candidate | `lux` |
| LTR-329 | `ltr329` | ltr303 | I2C 0x29 | auto | `lux` `full` `ir` |
| LTR390 | `ltr390` | ltr390uv | I2C 0x53 | auto | `uvi` `lux` |
| MAX44009 | `max44009` | gy49 | I2C 0x4A, 0x4B | auto | `lux` |
| OPT3001 | `opt3001` |  | I2C 0x44, 0x45, 0x46, 0x47 | auto | `lux` |
| Si1145 | `si1145` | si1146, si1147, gy1145 | I2C 0x60 | auto | `uvi` `visible` `ir` |
| TCS34725 | `tcs34725` | tcs34727, tcs3472 | I2C 0x29 | auto | `lux` `cct` `r` `g` `b` `clear` |
| TSL2561 | `tsl2561` | gy2561, tsl2560 | I2C 0x39, 0x29, 0x49 | auto | `lux` `full` `ir` |
| TSL2591 | `tsl2591` |  | I2C 0x29 | auto | `lux` `full` `ir` |
| VEML6075 | `veml6075` |  | I2C 0x10 | auto | `uvi` `uva` `uvb` |
| VEML7700 | `veml7700` | veml6030 | I2C 0x10, 0x48 | auto | `lux` `white` |

### Motion and compass (Movimiento y brújula)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| ADXL345 | `adxl345` | adxl343, gy291 | I2C 0x53, 0x1D | auto | `ax` `ay` `az` |
| BMI160 | `bmi160` | bmx160 | I2C 0x68, 0x69 | auto | `ax` `ay` `az` `gx` `gy` `gz` `temp` |
| BNO055 | `bno055` |  | I2C 0x28, 0x29 | auto | `heading` `pitch` `roll` `ax` `ay` `az` `gx` `gy` `gz` `temp` `calib` |
| HMC5883L | `hmc5883l` | gy273, gy85 | I2C 0x1E | auto | `mx` `my` `mz` `heading` |
| ICM-20948 | `icm20948` |  | I2C 0x69, 0x68 | auto | `ax` `ay` `az` `gx` `gy` `gz` `temp` `mx` `my` `mz` `heading` |
| ICM-42688 | `icm42688` | icm42670, icm42605 | I2C 0x68, 0x69 | auto | `ax` `ay` `az` `gx` `gy` `gz` `temp` |
| LIS3DH | `lis3dh` | lis2dh12 | I2C 0x18, 0x19 | auto | `ax` `ay` `az` |
| LIS3MDL | `lis3mdl` |  | I2C 0x1C, 0x1E | auto | `mx` `my` `mz` `heading` |
| LSM6DS | `lsm6ds` | lsm6ds3, lsm6dsl, lsm6dso, lsm6dsox, ism330 | I2C 0x6A, 0x6B | auto | `ax` `ay` `az` `gx` `gy` `gz` `temp` |
| MMC5603NJ | `mmc5603` |  | I2C 0x30 | auto | `mx` `my` `mz` `heading` `temp` |
| MMC5983MA | `mmc5983` |  | I2C 0x30 | auto | `mx` `my` `mz` `heading` `temp` |
| MPU-6050 | `mpu6050` | mpu6500, mpu9250, mpu9255, mpu6886, icm20602, icm20608, icm20689, gy521, gy91, gy9250 | I2C 0x68, 0x69 | auto | `ax` `ay` `az` `gx` `gy` `gz` `temp` (`mx` `my` `mz` `heading`) |
| QMC5883L | `qmc5883l` | gy271 | I2C 0x0D | auto ¹ | `mx` `my` `mz` `heading` |
| QMC5883P | `qmc5883p` |  | I2C 0x2C | auto | `mx` `my` `mz` `heading` |
| QMI8658 | `qmi8658` |  | I2C 0x6B, 0x6A | auto | `ax` `ay` `az` `gx` `gy` `gz` `temp` |

### Voltage, current and ADCs (Tensión, corriente y ADC)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| ADS1015 | `ads1015` |  | I2C 0x48, 0x49, 0x4A, 0x4B | declared ² | `a0..a3` |
| ADS1115 | `ads1115` |  | I2C 0x48, 0x49, 0x4A, 0x4B | auto | `a0..a3` |
| HX711 | `hx711` |  | GPIO (2 pins) | declared | `weight` `raw` |
| INA219 | `ina219` |  | I2C 0x40, 0x41, 0x44, 0x45 | auto | `bus` `current` `power` `shunt` |
| INA226 | `ina226` |  | I2C 0x40, 0x41, 0x44, 0x45 | auto | `bus` `current` `power` `shunt` |
| INA228 | `ina228` | ina238, ina237 | I2C 0x40, 0x41, 0x44, 0x45 | auto | `bus` `current` `power` `energy` `die` `shunt` |
| INA260 | `ina260` |  | I2C 0x40, 0x41, 0x44, 0x45 | auto | `bus` `current` `power` |
| INA3221 | `ina3221` |  | I2C 0x40, 0x41, 0x42, 0x43 | auto | `v1` `i1` `v2` `i2` `v3` `i3` |
| MAX17048 | `max17048` | max17043, max17049, max17044 | I2C 0x36 | auto | `soc` `cell` `rate` |
| MCP3008 | `mcp3008` | mcp3004 | SPI | declared | `a0..a7` |
| MCP3208 | `mcp3208` | mcp3204 | SPI | declared | `a0..a7` |
| MCP3421 | `mcp3421` | mcp3422, mcp3423, mcp3424, mcp3425, mcp3426, mcp3427, mcp3428 | I2C 0x68, 0x69, 0x6A, 0x6B | declared | `a0..a3` |
| MCP9600 | `mcp9600` | mcp9601 | I2C 0x67, 0x60, 0x65, 0x66 | auto | `temp` `cj` |
| NAU7802 | `nau7802` |  | I2C 0x2A | auto | `weight` `raw` |
| PCF8591 | `pcf8591` | yl40, yl-40 | I2C 0x48, 0x49, 0x4A, 0x4B | declared | `a0..a3` |
| PZEM-004T | `pzem004t` | pzem | UART | declared | `bus` `current` `power` `energy` `freq` `pf` `alarm` |

### Distance and presence (Distancia y presencia)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| HC-SR04 | `hcsr04` | sr04, hcsr04p, jsnsr04t, us100, aj_sr04m | GPIO (2 pins) | declared | `dist` |
| LD2410 | `ld2410` | ld2410b, ld2410c | UART | declared | `presence` `move` `still` `dist` `move_dist` `still_dist` `move_en` `still_en` |
| LD2450 | `ld2450` |  | UART | declared | `presence` `people` `dist` `x1` `y1` `speed1` … `x3` `y3` `speed3` |
| PIR | `pir` | hcsr501, am312, rcwl0516 | GPIO | declared | `motion` |
| RCWL-9620 | `rcwl9620` | m5ultrasonic, rcwl1601 | I2C 0x57 | declared | `dist` |
| TF-Luna | `tfluna` |  | I2C 0x10 | declared | `dist` `signal` `die` |
| Ultrasonido | `ping` | grove_ultrasonic, parallax_ping | GPIO | declared | `dist` |
| URM09 | `urm09` |  | I2C 0x11 | auto | `dist` `temp` |
| VL53L0X | `vl53l0x` | gy530 | I2C 0x29 | auto | `dist` |
| VL53L1X | `vl53l1x` |  | I2C 0x29 | auto | `dist` |
| VL6180X | `vl6180x` |  | I2C 0x29 | auto | `dist` `lux` |

### Other (Otros)

| Chip | Module id | Also known as | Bus | Found | Values |
|---|---|---|---|---|---|
| AS5600 | `as5600` | as5600l | I2C 0x36, 0x40 | declared | `angle` `magnet` |
| Caudal | `flow` | yfs201, yfs401, fs300a | GPIO | declared | `flow` `volume` |
| Entrada | `input` | contact, button, reed, switch | GPIO | declared | `state` |
| Geiger | `geiger` | radiationd | GPIO | declared | `cpm` `dose` |
| MAX3010x | `max3010x` | max30102, max30105, max30101, max30100 | I2C 0x57 | auto | `red` `ir` `die` (`hr`) |
| MPR121 | `mpr121` |  | I2C 0x5A, 0x5B, 0x5C, 0x5D | auto | `touch` |
| PCF8574 | `pcf8574` | pcf8574a | I2C 0x20, 0x38, 0x27, 0x3F | declared | `p0..p7` |
| Pulsos | `pulse` |  | GPIO | declared | `freq` `count` |
| Seesaw | `seesaw` | soil | I2C 0x36, 0x37, 0x38, 0x39 | auto | `moist` `temp` |

## The units

Every value has one unit, whatever the chip: °C, %RH, hPa, lx, V, A, W, ppm
(CO₂), ppb (TVOC), µg/m³ (particles), mm (distance and rain), g (weight
and acceleration in g), °/s, µT, degrees (heading, angles), %, Ω, Hz,
L/min, L, m/s, µSv/h, Wh, and 0/1 for a state. An index (air quality, VOC,
UV) has none. "Raw" values are the chip's own counts. The UIs scale them
for the eye (mV, mA, kΩ, m) and use the decimal comma.

## Module line arguments

Most chips need nothing. These take something after the address:

| Argument | Chips | What it is |
|---|---|---|
| `shunt=` | INA219, INA226, INA228/238, INA3221 | The shunt in milliohms (100: the R100 on most boards). INA3221 also `shunt1=` .. `shunt3=` |
| `gain=` | ADS1015/1115, MCP342x | Full scale in mV (ADS: 6144, 4096, 2048, 1024, 512, 256) |
| `gain=` | HX711 | 128 (channel A), 64 (A) or 32 (B) |
| `scale=`, `tare=` | HX711, NAU7802 | Counts per gram and the counts with nothing on it: then `weight` appears |
| `vref=` | PCF8591, MCP3008, MCP3208 | The reference voltage (3.3) |
| `dac=` | PCF8591 | Keeps its analog output on at 0..255 |
| `ch=` | MCP3008, MCP3208 | How many inputs to read (4 for an MCP3004/3204) |
| `bits=` | LM75 family, MCP342x | Resolution (LM75 9..12, MCP342x 12/14/16/18) |
| `model=` | MS5611/MS5607, MCP342x | Which one, when the chip cannot say (`model=5607`) |
| `type=` | MAX31856, MCP9600 | Thermocouple type, K by default (B E J K N R S T) |
| `avg=`, `mains=` | MAX31856, MAX31865 | Averaging; 50 or 60 Hz filter |
| `wires=`, `r0=`, `rref=` | MAX31865 | 2/3/4 wires; PT100 (100, 430) or PT1000 (1000, 4300). A module named `pt1000` takes the second |
| `filter=` | MCP9600 | Its filter, 0..7 |
| `heat=`, `heat_ms=` | BME680/688 | Gas heater temperature (200..400 °C) and time |
| `oss=` | BMP180 | Oversampling 0..3 |
| `g=`, `dps=` | accelerometers, gyroscopes | Range: 2/4/8/16 g, 250/500/1000/2000 °/s (8 g and 2000 °/s by default) |
| `xtal=1` | BNO055 | Use the board's crystal |
| `budget=` | VL53L0X | Timing budget in ms, 20..100 |
| `als=0` | VL6180X | No ambient light |
| `press=`, `comp=` | SCD30, SCD4x | Pressure in hPa, or another sensor's `press` |
| `comp=` | SGP30, SGP4x, CCS811, ENS160 | Another sensor's `temp` and `hum` for compensation |
| `raw=1` | SGP30, SGP4x | The raw signals too |
| `temp=`, `comp=` | HC-SR04, ping | Air temperature for the speed of sound, or a sensor's `temp` |
| `pull=`, `invert=1` | input, pir, pulses | `up`, `down` or `none`; the other sense |
| `k=` | flow | Pulses a second per L/min (7.5 for a YF-S201) |
| `mm=` | rain | Millimetres per tip (0.2794) |
| `factor=` | wind, geiger | m/s per Hz (0.667); µSv/h per CPM (0.00812, a J305 tube) |
| `window=`, `debounce=`, `edge=both` | pulses | Seconds for the rate; µs between edges; count both edges |
| `addr=` | PZEM-004T, Senseair S8 | Its Modbus address, when not the general one |

## Sensors on GPIOs

`port gpio.<name> pin=<n>` makes a pin a port; the module line on it says
what is there, and `pin2=` gives a second pin. The service claims the pins
while the sensor exists and frees them when its line goes.

- **DHT11, DHT22 (AM2302) and kin:** one pin, with a 4.7-10 k pull-up to
  3V3 (the internal one does for a short cable). A read every 2 s, as they
  want. The bits are timed by a GPIO interrupt (a few µs, the same for every
  edge) and the checksum is checked.
- **HC-SR04 (JSN-SR04T, US-100 in pulse mode, HC-SR04P):** `pin=` TRIG,
  `pin2=` ECHO. A 10 µs pulse, and the echo's width is the distance, 343 m/s
  at 20 °C. Nothing within 30 ms is "out of range". The 5 V HC-SR04's ECHO
  needs a divider (1 k and 2 k); the HC-SR04P runs at 3.3 V. **ping** is
  the one-pin kind (Grove Ultrasonic Ranger, Parallax PING))).
- **HX711:** `pin=` the clock (SCK), `pin2=` the data (DT). 24 bits at 10
  readings a second; each clock is a microsecond with interrupts held off
  only for that, because a clock longer than 60 µs powers it down.
- **Pulses** (`pulse`, `flow`, `rain`, `wind`, `geiger`): counted by the
  P4's PCNT, or by an interrupt with a debounce for reed switches (a rain
  gauge, an anemometer: 10 and 2 ms by default). The rate comes from the
  last few seconds (a minute for a Geiger tube).
- **input** and **pir:** a pin's level, 0 or 1. A contact to GND with the
  pull-up reads 1 open; `invert=1` makes "closed" 1. A PIR is high while
  it sees movement.

Apps can time pins the same way: `aos_io_edges_*`, `aos_io_pulse_out`,
`aos_io_shift_in` and `aos_io_count_*` in `aos_io.h`.

## Serial sensors

`module <id> uart.a`. The service opens the port at the sensor's speed for
each read and closes it, so the Terminal can use the port between reads.
TX of the sensor goes to the port's RX and RX to TX. Frames are checked
(checksum or CRC): a sensor at the wrong speed, or a different one, says so
("No se entiende lo que manda"), and so does silence ("No contesta por el
puerto serie").

- **MH-Z19B/C/E, Senseair S8:** CO₂ by NDIR, read every 2 s. P4OS never
  sends a calibration command.
- **PMS5003/7003/A003/3003, SDS011:** particles, whether the sensor
  streams or waits to be asked; P4OS never changes its mode.
- **PZEM-004T v3.0:** mains voltage, current, power, energy, frequency and
  power factor over Modbus. **Its terminals carry mains voltage.** The
  serial side is isolated; the other side is not a bench toy.
- **LD2410 (B, C), LD2450:** human presence by 24 GHz radar at 256000 baud.
  The LD2450 gives up to three people's positions (x, y in mm) and speeds.

## In the simulator

Every chip has an emulated twin that speaks its protocol, with its timing,
CRCs and power-on values, and readings that drift slowly:

- `P4_SIM_SENSORS="bme688@0x77,tsl2561@0x39,mpu9250,ak8963"`: I2C chips
  on every port but the board's (one per address). The model names are in
  `sim/emu_*.c` and `sim/sensors_sim.c`.
- `P4_SIM_SPI_A="48=max31855,47=mcp3208,46=max31865"`: SPI chips by CS.
- `P4_SIM_GPIO_SENSORS="2=dht22,28=hcsr04,34=hx711,46=pulse:15"`: on the
  pin the driver reads (the echo, the data).
- `P4_SIM_UART_A=sensor:mhz19` (and `_B`): a serial sensor on the port.

## For apps

- `aos_sensor_count()`, `aos_sensor_at()` and `aos_sensor_value(name,
  key, &v)` as before; `aos_sensor_values(i, out, max)` gives all of a
  sensor's values (up to 12), each with its key and quantity
  (`AOS_SQ_*`), so an app knows the unit without knowing the chip.
- `aos_sensor_text.h` says them the way Módulos does: the key's name, a
  glyph, the value with its unit and the decimal comma, the error in words,
  what a chip measures, and the form's groups (`aos_sensor_chip_cat()`).

## For a new driver

A driver is a `sens_drv_t` in one of the family files
(`components/aos_io/sensors/drv_<family>.c`) and its emulated twin in
`sim/emu_<family>.c`. `sens_int.h` has the contract: `identify()` only
reads, `init()` once, `read()` every round or every `min_ms`, values with
`sens_setv()`, state in the slot's 320 bytes (`SPRIV`), and `fini()` for
what is not the bus's. The table puts every family's drivers in priority
order; at a shared address the order is the safety.
