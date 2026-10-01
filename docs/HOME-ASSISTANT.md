# Home Assistant on P4OS

The Home Assistant app talks to HA's WebSocket API directly: live states, no
polling, service calls from the tiles.

## Connecting

1. In Home Assistant, open your profile, then **Security**, then **Long-lived
   access tokens**, and create one.
2. Put a file named `ha.txt` at the root of the microSD card:

   ```
   url=http://192.168.1.10:8123
   token=eyJhbGciOiJIUzI1NiIsInR5cCI6...
   favs=light.living,switch.coffee,climate.living
   ```

   `favs` is optional: the entities for the Favourites page and the home
   widget, in order.
3. Open the app (or restart). P4OS moves the token into its preferences and
   rewrites that line of the file as `token=(guardado en P4OS)`, so it does
   not stay on the card in the clear. The URL can also be edited in the app
   (the gear, then **Dirección**).

Both `http://` (HA's local address) and `https://` (Nabu Casa, or a reverse
proxy with a real certificate) work; https needs the clock set by the
network first, since the certificate is checked against the board's bundle
of trusted authorities. A self-signed certificate is refused.

## What it shows

- Areas as tabs (a sidebar in landscape), plus Favourites and Others.
- Controls: lights, switches, input booleans, fans, covers, climates, locks,
  media players, scenes, scripts and buttons. A tap does the obvious; a long
  press opens a sheet with brightness, position, fan speed or climate target
  and mode, and the star that adds it to Favourites.
- Sensors and binary sensors, live.

Hidden entities and config/diagnostic ones are left out. An entity's area is
its own or, failing that, its device's, as in HA's own dashboards.

A sensor's sheet also draws its history: the last 6 hours, 24 hours or
7 days, from HA's recorder.

The home screen widget (`widget ha 4x2` in `menu.txt`) shows the first four
favourites; tapping one toggles it. The Control Centre's Home Assistant tile
runs up to four scenes and scripts: the favourite ones first, then the
house's other scenes.

## Notifications

Two things reach P4OS's notification centre, as banners from
"Home Assistant":

- HA's own persistent notifications (the bell in HA's sidebar): the ones
  already there when P4OS connects, and every new one. Dismissing one in HA
  removes it here, and dismissing it here (its ✕ in the notification centre,
  or "Borrar todo") dismisses it in HA, so it goes from every other screen
  too.
- An event made for this, `p4os_notify`, which any automation can fire:

  ```yaml
  action:
    - event: p4os_notify
      event_data:
        title: Lavarropas
        message: Terminó el lavado.
  ```

## Developing without a real HA

`tools/fake_ha.py` is a stand-in with a house full of entities, sensors that
drift and services that work:

```
tools/fake_ha.py --port 8123 --token test-token
```

Then in `sim/sim_fs/ha.txt`: `url=http://127.0.0.1:8123` and
`token=test-token`. With `--tls --cert --key` it serves wss:// instead; the
recipe for a throwaway CA is in its docstring, and `P4_SIM_EXTRA_CA=ca.pem`
makes the simulator trust it.
