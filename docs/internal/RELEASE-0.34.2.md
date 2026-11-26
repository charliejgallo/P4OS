# Notas de la release 0.34.2

Las notas de la release `v0.34.2`, del 2026-11-26. El texto de abajo, en
inglés como las demás, es el que va a GitHub.

---

P4OS 0.34.2 for the [Waveshare ESP32-P4-WIFI6-Touch-LCD-5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm). **The icon packs with Vision, and a warning about the C6's firmware.** [CHANGELOG.md](https://github.com/charliejgallo/P4OS/blob/v0.34.2/CHANGELOG.md) has every change; [0.34.0's notes](https://github.com/charliejgallo/P4OS/releases/tag/v0.34.0) tell what that release brought (Zigbee and drawn icons) and how to update the radio's firmware.

## What's new

- **Both icon packs have Vision's icon**: 3D toy and Line now draw 104 apps. If you have a pack, Store → Updates offers it again.
- **A warning before installing the C6's firmware** ([issue #2](https://github.com/charliejgallo/P4OS/issues/2)). P4OS's C6 firmware is esp_hosted 3.0.9, and a host older than esp_hosted 3 cannot talk to a 3.x C6. So if you also run other firmware on this board (bmorcelli's Launcher, Arduino sketches, Espressif demos built on esp_hosted 2.x), they find no Wi-Fi after it. P4OS works with either. Settings → Update says so next to "Install on the C6", and the Store's entry for the radio's firmware too. The way back, from P4OS with no cable, is in [docs/C6.md, "Other firmware on the board"](https://github.com/charliejgallo/P4OS/blob/v0.34.2/docs/C6.md#other-firmware-on-the-board).

## Updating

Settings → Update → From GitHub: 0.34.2. Then Store → Updates: the icon packs you have. The radio's firmware did not change.

## Tried on the author's board

- Both packs with Vision copied to the card: the home screen draws them; with the Line pack, Planeo opened with 15 MB free (the apps waiting in the background closed first) and made its world in 3.2 s.

## The files

As in 0.34.1. The two icon packs are in [v0.34.1](https://github.com/charliejgallo/P4OS/releases/tag/v0.34.1), where the Store fetches them from; the other System parts stay in [v0.33.0](https://github.com/charliejgallo/P4OS/releases/tag/v0.33.0); the Translator's language packs in [v0.31.0](https://github.com/charliejgallo/P4OS/releases/tag/v0.31.0).

---

## Cómo se hizo

- A pedido del usuario ("sacá una release 0.34.2 con los packs nuevos"), el 2026-11-26.
- Los dos packs subidos a `v0.34.1` y anotados en `store/packs.json` (sólo esos dos; `c6.bin` queda en `v0.33.0`).
- `main` en `ed145ebf` y su CI, verde; tag `v0.34.2` y su CI (borrador con
  71 archivos). Los 457 archivos que nombra el catálogo, sin un error; los
  packs de íconos en `v0.34.1` con el SHA nuevo, `c6.bin` en `v0.33.0`; las
  dos imágenes dicen `v0.34.2`. Publicada el mismo día.
