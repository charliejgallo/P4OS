# Bluetooth

The P4 has no radio. NimBLE's host runs on the P4 and its HCI goes to the
ESP32-C6's controller through esp_hosted, over the same SDIO link as the
Wi-Fi (`esp_hosted_bt_host_stack_setup`, `components/aos_ble/aos_ble.c`).
The C6 needs esp_hosted's BT feature: the factory firmware has it, and so
does `c6/` (docs/C6.md).

The stack came from AmoledOS, the smartwatch this project descends from:
advertising, pairing, the iPhone's notifications (ANCS) and music (AMS),
its battery and its time. What is new here is the base, the C6, and the
keyboard mode.

It is off by default, like the watch's. Settings, Bluetooth switches it on;
so does `POST /api/bt {"on": true}`.

## The phone

- **Pairing** is by numeric comparison: the iPhone and the board show the
  same six digits, and the board's code comes up over everything, the lock
  screen included (`aos_pair_ui.c`), until it is answered. The keys are kept
  in NVS, and the phone comes back by itself.
- **Showing up in the iPhone's list.** iOS lists in Settings, Bluetooth only
  accessories of the kinds it handles there. A plain BLE peripheral is found
  by apps such as LightBlue and never by Settings, which is what happened to
  the watch. The board advertises the HID service UUID (0x1812) next to the
  ANCS solicitation, as Espressif's own ANCS example does, and iOS lists it.
  The 31-byte packet holds flags, the HID UUID, a name of up to 4 characters
  and the solicitation; the whole name and the appearance (a watch) go in the
  scan response.
- **Notifications** arrive through ANCS into the same store the simulator
  uses (`aos_notif.c`): banners, the notification centre and the lock
  screen, with colour emoji (docs/EMOJI.md).
- **The phone's battery and time** come from the Battery and Current Time
  services the iPhone publishes. The time is applied from the main loop,
  never from NimBLE's task.
- **Music (AMS)** is off by default, as on the watch: Settings, Bluetooth,
  "Música del iPhone", or `POST /api/bt {"music": true}`. With it on, the
  control centre and the lock screen show what the phone plays ("Artist ·
  Spotify") and their buttons drive it. One row, two sources
  (`aos_nowplaying.c`): the board's own player when it is playing, since it
  is what the speaker says; else the phone with a track, playing or paused;
  else the board, paused. The buttons go to the one shown, chosen again at
  the tap.
- **The notifications' actions** are the phone's own, and only the ones it
  declared get a button (`can_positive` and `can_negative` are independent:
  a call brings both, a WhatsApp message only the negative one). In the
  notification centre a card says "Rechazar" / "Atender" for a call and
  "Borrar en el teléfono" for the rest. The phone answers by withdrawing the
  notification, which takes the card away; if it could not, it says so and
  a toast tells (`aos_notif_act`).
- **An incoming call** takes the whole screen, over the lock screen too:
  the caller, Reject and Answer, a beep every two seconds if it sounds, and
  the screen kept on. It closes when the phone withdraws the call (answered
  on either side, rejected, or given up) and after 90 s at most.

## The keyboard mode

With "Bluetooth keyboard" on (Settings, Bluetooth, or
`POST /api/bt {"keyboard": true}`), the board is a computer's keyboard,
mouse and media keys: HID over GATT (`aos_ble_hid.c`), with the Device
Information (PnP ID) and Battery services HOGP asks for. The reports are the
USB KEYS mode's: a keyboard (id 1), the consumer control (id 2, the media
keys) and a mouse (id 3). The services are registered only with the mode on,
before the stack starts, so switching it restarts Bluetooth.

- **Pairing** from the Mac's or the PC's own Bluetooth settings, with the
  same numeric comparison.
- **Two connections at once,** the phone and the computer. A connection is
  the phone if it has ANCS and the computer if it has not; until that is
  known it waits in the phone's place, and the two are swapped if the phone
  came second. A third is closed.
- **The phone is refused the HID characteristics.** An iPhone that takes the
  board for a keyboard hides its own on-screen one. It is recognised by ANCS
  on its connection, and by its identity address on the next one.
- **The same API as the cable.** `aos_hal_usb_key`, `_type`, `_mouse`,
  `_click` and `_mouse_hold` go over Bluetooth when no computer has the USB
  port in KEYS mode (the cable wins when there are both). The Macro pad, the
  portal and the apps send keys without knowing which way they go, and the
  Macro pad's chip says BLE with a blue dot. The gamepad and MIDI stay on the
  cable.
- **Full queues.** A text is two notifications a character, faster than one
  connection interval carries them. When NimBLE runs out of buffers a report
  waits for room and tries again (releases too, or a key would stay down).
- **The computer's name** is read after an MTU exchange: before it a read
  carried 19 bytes, and a Mac ended the long read one byte short.

Tested with an iPhone 15 Pro Max and a MacBook Air at once: a 72-character
text, volume up, the Macro pad's trackpad, and both coming back by
themselves after the board restarts.

## The portal

    GET  /api/bt                          state, phone, battery, keyboard, computer,
                                          music, and what the phone plays
    POST /api/bt {"on": true|false}
    POST /api/bt {"keyboard": true|false}
    POST /api/bt {"forget": true}         wipes the phone's keys
    POST /api/bt {"key": "volup"}         through the keyboard, cable or Bluetooth
    POST /api/bt {"type": "some text"}
    POST /api/bt {"music": true|false}    the iPhone's music (AMS)
    POST /api/bt {"media": "play"}        play/pause, "next", "prev" on the phone

## Memory

6 KB of internal RAM in the firmware (4.5 KB of IRAM, the rest .bss), and
7 KB more with Bluetooth on, 5 of them the stack of NimBLE's host task. That
stack stays internal: the task writes the phone's keys to NVS, and a PSRAM
stack cannot be used while the flash is written. NimBLE's own memory is in
PSRAM (`BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`).
