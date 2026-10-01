# p4bench — primer día con la placa

Firmware de pruebas para la Waveshare ESP32-P4-WIFI6-Touch-LCD-5. Implementa las
pruebas de `docs/plan/HARDWARE.md` y deja cada resultado como una línea
`BENCH <prueba> clave=valor ...`, que `tools/bench_run.py` junta en
`bench/results/`.

## Qué hay

| Carpeta | Qué es |
|---|---|
| `p4bench/` | El firmware. `build.sh <perfil> <variante>` compila en `build/<perfil>-<variante>/` |
| `hello_so/` | El `.so` de prueba del cargador ELF (`elf so bench/hello_so.so`) |
| `results/` | Lo medido. `raw/` queda local (tiene IPs y números de serie) |

Perfiles de chip: `rev3_x` (lo más probable) y `rev1_3`. Variantes de LVGL:

| Variante | LVGL | Para qué |
|---|---|---|
| `lvA` | sin sistema operativo, 1 draw unit | lo que usa AmoledOS |
| `lvB` | FreeRTOS, 2 draw units (una por núcleo) | **la candidata** |
| `lvC` | lvB + el PPA como draw unit | ver si el PPA suma |

La configuración de LVGL es ABI de las apps: la variante que gane la prueba 5 es
la que se congela para P4OS.

## Orden del primer día

Enchufar el USB-C que dice **UART** (el CH343; el otro es el OTG).

```bash
# 0. qué chip vino (no escribe nada)
source ~/esp/esp-idf/export.sh
esptool.py --chip esp32p4 -p /dev/cu.usbmodem* chip_id
esptool.py --chip esp32p4 -p /dev/cu.usbmodem* flash_id

# 1. respaldo completo del firmware de fábrica (32 MB, tarda unos minutos)
esptool.py --chip esp32p4 -p /dev/cu.usbmodem* -b 921600 read_flash 0 0x2000000 bench/results/raw/factory-full.bin

# 2. grabar p4bench (perfil según lo que dijo chip_id)
bench/p4bench/build.sh rev3_x lvB -p /dev/cu.usbmodem* flash

# 3. la tanda sin intervención
tools/bench_run.py first

# 4. red (guardar la red una vez: queda en NVS y conecta sola al arrancar)
tools/bench_run.py cmd "wifi set <ssid> <clave>" "wifi up"
tools/bench_run.py net

# 5. LVGL, una tanda por combinación (cada cfg reinicia la placa)
tools/bench_run.py cmd "lvgl cfg rot=0 copy=ppa fbs=2" reboot
tools/bench_run.py lvgl
tools/bench_run.py cmd "lvgl cfg rot=90 copy=ppa" reboot
tools/bench_run.py lvgl
tools/bench_run.py cmd "lvgl cfg copy=cpu" reboot
tools/bench_run.py lvgl
#    ...y lo mismo grabando lvA y lvC

# 6. con alguien frente a la placa
tools/bench_run.py hands

# 7. el header, con jumpers en las filas 11-12, 15-16, 21-22, 23-24, 31-32, 35-36, 37-38
tools/bench_run.py cmd "gpio pairs"
tools/bench_run.py cmd "gpio watch 60"      # tocar cada pin con un cable a GND
tools/bench_run.py cmd "gpio vo4"           # y medir pines 35/37/39 con el multímetro

# 8. el cargador ELF
cp bench/hello_so/build/hello_so.so /Volumes/<SD>/bench/
tools/bench_run.py cmd "elf so bench/hello_so.so 1000000"
```

Una vez en WiFi, todo lo anterior también anda sin cable:
`tools/bench_run.py --http p4bench.local <suite>`.

## Comandos (ver `help` en la consola)

| Grupo | Comandos | Prueba |
|---|---|---|
| sistema | `info` `psram` `temp` `load [s]` `tasks` `rtc set/check` `bat` `cfg` `reboot` | 0, 2, 17, 18, 20 |
| pantalla | `lcd info/rate/bars/pattern/fill/tear/bl/blsweep/on/off/fbs` | 3, 4 |
| LVGL | `lvgl bench/widgets/music/ui/stats/cfg` | 5, 6 |
| táctil | `touch info/int/rst/rate/paint` | 7 |
| microSD | `sd info/mount/umount/speed/ls` | 8 |
| red | `wifi set/up/scan/status/soak` `ntp` `tls` `hosted fw/ota` `iperf` | 9 |
| audio | `audio tone/sweep/wav/vol` `mic` `duplex` | 12 |
| motores | `jpeg` `ppa` | 13 |
| header | `gpio map/watch/pairs/loop/set/vo4` `uart loop` `i2c scan` `mb` | 15 |
| ELF | `elf so` | 19 |

La prueba 10 (ESP-NOW) ya se resolvió sin placa: esp_hosted 1.4.7 no lo trae.
Faltan todavía: BLE (11) y USB OTG (14), que necesitan su propia configuración
y van en una segunda versión del banco.

## RS485 y Modbus en el banco

Con la placa RS485 en el perfil por defecto (TX 30, RX 31, DE 29):

```
mb 30 31 29 9600 1 3 0 2       # esclavo 1, holding registers 0-1
mb 30 31 -1 115200 1 3 0 4     # placa con dirección automática (sin DE)
```
