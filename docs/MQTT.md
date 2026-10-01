# The MQTT app

MQTT is a window on a broker: every topic that goes by, as a tree you can
fold, with the last message of each one, a chart of every number in it, and
a way to publish. Underneath is a service, `aos_mqtt.c`, that stays connected
after the app closes, so other apps (the Macro pad, later) can publish
through it.

It speaks MQTT 3.1.1 over plain TCP. There is no TLS, so it is for brokers on
the local network (Mosquitto, the Home Assistant add-on, EMQX...).

## Setting it up

Use the **Conexión** tab. Tap each row and type on the screen's keyboard:

- **Servidor**: `host:port` (the port is 1883 if you leave it out);
- **Usuario** and **Contraseña**, if the broker asks for them;
- **ID de cliente**: leave it empty and P4OS makes one up once
  (`p4os-1a2b`), then keeps it;
- **Suscripción**: `#` (everything) by default, or several filters separated
  by commas (`zigbee2mqtt/#, home/#`);
- **Keepalive**: 15, 30, 60 or 120 s.

A password is awkward to type on a screen, so the settings can also come from
a file `mqtt.txt` at the root of the card:

    host=192.168.1.10
    port=1883
    user=p4os
    password=secret
    client_id=p4os-bench
    sub=zigbee2mqtt/#, home/#

P4OS reads it at start and every few seconds while there is no broker set.
**Leer mqtt.txt de la tarjeta** reads it again. The password moves into the
preferences, and its line in the file is rewritten as
`password=(guardada en P4OS)`.

The switch on the state card turns the client off and on. If the broker
refuses the user or the password, P4OS does not retry until the settings
change. Any other failure is retried after 1, 2, 5, 10 and then every 30 s,
and the subscriptions are sent again on every new connection.

## Explorar

The topics form a tree by level: `zigbee2mqtt` › `living_temp`. A folded level
says how many topics it holds. A row flashes when a message lands in it, or
anywhere under it while it is folded. The dot on a topic gives its kind:
violet for a number, cyan for JSON and grey for text. The pin means that the
last message came retained.

The button beside the search unfolds every level, or folds them all again.
Typing in the search turns the tree into a flat list of the topics whose name
contains that text.

Tap a topic to see its detail. In portrait it opens as a sheet, and in
landscape it fills the panel on the right. It shows:

- the facts: retained or not, QoS, how many messages, when the last one came
  and how big it was;
- the numbers: a payload that is a number is charted as it is. For a JSON
  object, each numeric field gets a chip (`temperature`, `ENERGY.Power`), and
  the chart shows the chosen one over its last 40 values. Booleans count as
  0 and 1;
- the message itself, re-indented if it is JSON. A payload longer than
  256 bytes is kept cut, and the sheet says its full size;
- **Publicar acá**, which fills the Publicar tab with this topic and message;
- **Mandar una orden**, when the topic belongs to a device P4OS knows how to
  drive. It fills the form with the device's command topic and a toggle:
  `zigbee2mqtt/<dev>/set {"state":"TOGGLE"}`, Tasmota's
  `cmnd/<dev>/POWER TOGGLE`, or ESPHome's `.../command TOGGLE`.

## Publicar

This tab has the topic, the message, QoS 0 or 1 and retain. The topic field
offers the topics seen, and the command topics it can guess from them
(violet) come first. QoS 1 waits for the broker's PUBACK and resends the
message with DUP after 10 s. A publish made while there is no connection
waits up to 30 s for one.

Each publish is kept in **Recientes** (ten of them, in the preferences):

- tap one to send it again;
- hold one to load it into the form;
- the pin keeps one at the top for good.

## The service, for other apps

```c
#include "aos_mqtt.h"

aos_mqtt_publish("zigbee2mqtt/desk_lamp/set", "{\"state\":\"TOGGLE\"}", 0, false);
aos_mqtt_subscribe("macropad/#", 0);          /* kept across reconnections */
```

The functions can be called from any task, LVGL's included. `publish` only
queues the message, eight at most. It returns false if there is no broker set
or MQTT is off, if the topic has `+` or `#`, if the payload is over 512 bytes
or if the queue is full.

To read the table, take `aos_mqtt_lock()`, then go through
`aos_mqtt_topic_at()`, `aos_mqtt_fields()` and `aos_mqtt_history()`, then
call `aos_mqtt_unlock()`. `aos_mqtt_version()` changes with every message,
which makes it a cheap test for "anything new?".

Memory: the table holds up to 512 topics. When it is full, the topic heard
from least recently makes room for a new one. It keeps 256 bytes of each
payload and 384 numeric series of 40 points, shared by all topics. All of it
is one block of about 360 KB in PSRAM, taken when the service starts. The
receive buffer is 8 KB. A packet larger than that is still counted: its head
(topic, packet id, the start of the payload) goes into the table and the rest
is skipped. The service uses one of the four `aos_hal_tcp_*` sockets for as
long as it is connected.

## The portal

The web portal has an **MQTT** page. It shows:

- the state and the settings;
- a publish form;
- the list of topics, with a filter;
- the detail of the topic you pick: the payload formatted and a chart of each
  field.

It works over `GET /api/mqtt`, `/api/mqtt/topics` and
`/api/mqtt/topic?t=<topic>`, and `POST /api/mqtt/publish`, `/config`,
`/reconnect` and `/clear` (`components/aos_portal/aos_portal_mqtt.c`).

## Testing without a house

`tools/fake_mqtt.py` is an MQTT 3.1.1 broker written with the standard library
only. It supports retained messages, `+` and `#`, QoS 0 and 1, keepalive, and
an optional user and password (`--user`, `--password`). It also plays a house
that publishes every second:

- zigbee2mqtt thermometers, a plug and a door;
- plain numbers under `home/`;
- a Tasmota plug under `tele/` and `stat/`;
- an ESPHome node;
- Home Assistant discovery configs, retained.

It obeys `zigbee2mqtt/kitchen_plug/set`, `cmnd/plug1/POWER` and
`garage-node/switch/relay/command`, and prints everything clients publish.

    tools/fake_mqtt.py --port 18883
    # in sim/sim_fs/prefs.txt: mqtt_host=127.0.0.1 and mqtt_port=18883
