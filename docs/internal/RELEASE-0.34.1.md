# Notas de la release 0.34.1

Las notas de la release `v0.34.1`, del 2026-11-25. El texto de abajo, en
inglés como las demás, es el que va a GitHub.

---

P4OS 0.34.1 for the [Waveshare ESP32-P4-WIFI6-Touch-LCD-5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm). **Memory for the big apps, and Planeo's worlds.** [CHANGELOG.md](https://github.com/charliejgallo/P4OS/blob/v0.34.1/CHANGELOG.md) has every change, and [0.34.0's notes](https://github.com/charliejgallo/P4OS/releases/tag/v0.34.0) what that release brought: the board as a Zigbee hub and drawn icons (**and how to update the radio's firmware**, if you have not yet).

## The fixes

- **Planeo could get stuck on "making the world"**, its world empty. The board had enough PSRAM free (15.8 MB) but in pieces, and the 2 MB block Planeo asks for while making a world did not fit any more. It now takes what the first world needs at the start, the biggest pieces first, and lets go of it once the world is made, so its peak is the same. Tried on the author's board: three worlds in a row, about 36 fps.
- **Planeo's free flight has every world from the start.** The missions still open the next world with stars.
- **The board learns what each big app takes.** While an app is in front the board notes the least PSRAM left free; for the apps that took 3 MB or more it keeps that number, and when one of them opens again with less free than it needs, the apps waiting in the background close first, except the busy ones (playing music, recording, serving the portal, downloading). Games already did this since 0.34.0.
- **The Store with an icon pack uses about half the memory:** its list draws the icons of the rows near the screen and lets go of the rest. "Everything" with some 80 apps: 3.3 MB before, 1.8 MB now.

## Updating

Settings → Update → From GitHub: 0.34.1. Then Store → Updates: **Planeo** has an update. Nothing else to do; the radio's firmware did not change.

## Tried on the author's board

- Planeo: islands, city and snow made one after the other (3.4 to 4 s each), about 36 fps; its peak learnt as 13 MB.
- The Store's "Everything" scrolled to the end and back: 3.3 MB before, 2.75 MB with a screen of icons each way, 1.8 MB with half a screen.

## The files

As in 0.34.0, with Planeo's package new. The System parts (the radio's firmware, the Zigbee tables, the icon packs) stay in [v0.33.0](https://github.com/charliejgallo/P4OS/releases/tag/v0.33.0); the Translator's language packs in [v0.31.0](https://github.com/charliejgallo/P4OS/releases/tag/v0.31.0).

---

## Cómo se hizo

- A pedido del usuario ("sacá una release 0.34.1 con estos arreglos"), el 2026-11-25.
- Sin packs nuevos; `store/packs.json` y `CARRIED_PACKS` no cambiaron.
- `main` en `9317cf11` y su CI, verde; tag `v0.34.1` y su CI (borrador con
  67 archivos). Los 457 archivos que nombra el catálogo, buscados en su
  release con su tamaño, sin un error; lo nuevo, el paquete de Planeo; las
  dos imágenes dicen `v0.34.1`. Notas cargadas y publicada el mismo día;
  `releases/latest` sirve su catálogo.
