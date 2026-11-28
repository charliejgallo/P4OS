# Notas de la release 0.35.1

Las notas de la release `v0.35.1`, publicada el 2026-11-27 (tag en `5c679576`). El texto de abajo, en
inglés como las demás, es el que va a GitHub.

---

P4OS 0.35.1 for the [Waveshare ESP32-P4-WIFI6-Touch-LCD-5](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-5.htm). **A fix for the Store.** [CHANGELOG.md](https://github.com/charliejgallo/P4OS/blob/v0.35.1/CHANGELOG.md) has every change, and [0.35.0's notes](https://github.com/charliejgallo/P4OS/releases/tag/v0.35.0) what that release brought: eight new apps, a Telegram bot, the phone as a gamepad and the App Library.

## The fix

- **The Store could run out of downloads.** Opening an app's page starts downloading its pictures, and each one held one of the board's two download slots until the page asked for it again. Leaving a page while its pictures were still coming kept both slots taken for good: after that, every install, update or "Replace" failed with "could not start the download (-2)", and the pictures of other pages never came. The slots of pictures that ended now go back at once, and an install takes the slot of a picture still coming. A restart also cleared it, which is the way round it on 0.35.0.

## Updating

Settings → Update → From GitHub: 0.35.1. Nothing else changes: no app, pack or System part is new.

## Tried on the author's board

Leaving Malla's and Grabador's pages with their pictures still coming, then "Replace" on the apps copied by hand: all twelve replaced by the Store's, with no download error, and the French, Italian and Portuguese system languages installed after them.

## The files

As in 0.35.0. The packs stay where 0.35.0's catalogue names them; the Translator's language packs in [v0.31.0](https://github.com/charliejgallo/P4OS/releases/tag/v0.31.0).

---

## Cómo se hizo

Sin packs nuevos. `main` y el tag a la vez (sólo cambiaba el servicio de la
Tienda y sus tests); CI del tag: borrador con 70 archivos. Los 194 archivos
que nombra el catálogo en su release con su tamaño y su SHA-256 (los dos
firmwares en `v0.35.1`, el resto como en la 0.35.0) y las 306 capturas, sin
un error; las dos imágenes dicen `v0.35.1`; "Nuevas" sigue en `v0.35.0`.
Publicada con el OK del usuario; `releases/latest` sirve su catálogo.
