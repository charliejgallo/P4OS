# Vision's engine

Espressif's [esp-dl](https://github.com/espressif/esp-dl) (MIT) as a module of
the app: `build/modules/engine.so`, which `tools/install_apps.sh` puts in the
card's `apps/vision/` and the app opens with `aos_hal_module_open()` when a
detector is chosen. The app talks to it through the C table of
`../main/ve_api.h`; `ve_engine.cpp` sets up each kind of detector as esp-dl's
own model components do (`esp-dl/models/<model>/<model>.cpp`: the same
preprocessing, anchors and thresholds).

| Kind | Models (from the packs, `../tools/mkpack.py`) | esp-dl's numbers on a P4 (model only) |
|---|---|---|
| Faces | MSR (120x160) + MNP (48x48 per candidate) | 12.4 ms + 2.3 ms each |
| People | Pico 224x224 | 45.9 ms |
| COCO, 80 classes | YOLO11n 320x320 | 312 ms |
| Hands and gestures | ESPDet-Pico 224x224 + MobileNetV2 128x128 | 45.3 ms + 23.8 ms each |

## Files

- `espdl/`: esp-dl at commit `512ed0b` (2026-11-06, component 3.3.15), the
  parts the P4 and the simulator use: `dl/` and `vision/` without the
  ESP32-S3 and Xtensa kernels, `spec/`, the FlatBuffers schema
  (`fbs_loader/espdl.fbs`) and the loader's headers, `LICENSE`. Not
  vendored: the audio, the BMP, JPEG and PPA helpers, the model loader
  (below) and its prebuilt library.
- `gen/p4`, `gen/c`: what esp-dl's CMake generates from `spec/*.yml` for a
  build with every operator, for the P4's SIMD kernels and for the plain C
  ones. `../tools/gen_espdl.py espdl gen/p4 p4` (and `c`) writes them again.
- `ve_fbs_model.cpp`: esp-dl's model reader, `fbs::FbsModel`, written here.
  esp-dl ships it only as `libfbs_model.a` built for the chips' flash: not
  position-independent (`ld` refuses it in a shared object) and not for the
  Mac. This one reads the FlatBuffer in place with a small reader of its
  own, from the schema and what esp-dl's callers expect.
- `ve_fbs_loader.cpp`: esp-dl's `FbsLoader` emptied (flash partitions, AES):
  the engine hands `dl::Model` an `FbsModel` over the app's buffer.
- `ve_rt.cpp` (board only): what esp-dl needs that the firmware does not
  export to apps: `operator new` into PSRAM, libstdc++'s error hooks,
  ESP-IDF's log, timer and heap queries, `__clzsi2`, `tanhf`, pthread
  mutexes over the HAL's.
- `rt/`: GCC 14.2.0's `tree.cc`, `list.cc`, `hashtable_c++0x.cc` (with
  `shared/hashtable-aux.cc`) and `hash_bytes.cc`, the parts of libstdc++
  behind `std::map`, `std::list` and `std::unordered_map`. GPL-3.0 with the
  GCC Runtime Library Exception (`COPYING3`, `COPYING.RUNTIME`), which lets
  them be linked into a module of any licence, as CAS does.
- `compat/` (simulator only): stand-ins for the ESP-IDF headers esp-dl
  includes.
- `board.cmake`, `sources.cmake`, `../sim.cmake`: the builds. On both the
  engine's objects become one object with `ve_module_api` its only global.

## Changes to esp-dl

Each is marked `P4OS` in the source.

- `dl/module/src/dl_module_base.cpp`: one core (`VE_SINGLE_CORE`). The
  dual-core runtime starts FreeRTOS tasks, which apps cannot reach; the
  firmware's worker split is the way to two cores later.
- `dl/base/isa/esp32p4/dl_esp32p4_s8_max_pool2d.S`: a `.type` named a
  function that does not exist, which left an undefined symbol.
- `dl/base/dl_base_conv_args.hpp`: the C kernels' per-channel shifts were
  never set (only the SIMD's were); for the simulator.
- `(unsigned)&x[0] & 15` became `(uintptr_t)&x[0] & 15` (alignment checks;
  the same on the P4, and they compile on a 64-bit Mac).
- `vision/image/dl_image.hpp`: without the BMP, JPEG and PPA helpers.

## The simulator

esp-dl's models are exported for one chip: for the P4, esp-ppq packs every
convolution's filter in blocks of the SIMD's lanes and stores 32-bit biases
where the C kernels add 16-bit ones. The simulator copies each model and
undoes that before esp-dl reads it (`unpack_simd_filters()`, compiled with
`VE_UNPACK_FILTERS`). Faces, people and hands find what the board would;
COCO's YOLO finds less (an operator of its other than the convolutions
still differs). `../tools/ve_check.sh --detect <kind> in.ppm out.ppm
models...` runs a detector on a picture on the Mac.

## Licences

esp-dl and its models: MIT (Espressif). YOLO11n (COCO) and ESPDet-Pico
(hands) were trained with Ultralytics' code, which is AGPL-3.0; Espressif
publishes the weights as MIT, and the packs with them say so in their
`info` entry. quirc (`../main/quirc`): ISC. libstdc++'s pieces: above.
