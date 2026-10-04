# P4OS's copy of ESP-IDF's USB host component

This is `components/usb` of ESP-IDF release/v5.5 (c94f345e), copied into the
project so it takes precedence over ESP-IDF's own (the first commit that
adds it is the copy unchanged; `git log -- components/usb` shows what P4OS
changed since). The license is ESP-IDF's, Apache-2.0, as each file says.

## Why

ESP-IDF's host library drives one root port. The ESP32-P4 has two USB
controllers (High Speed and Full Speed), and P4OS uses both at once: a
keyboard on one and a pendrive on the other (docs/USB.md, "The pendrive
host").

## What changed

- `hub.c`: one root port per USB-OTG peripheral in `port_map`, each with its
  own state and requests. A device on a root port has `parent_dev_hdl` NULL
  and `parent_port_num` = the root port's index (the peripheral's number);
  the checks for "a root port" look at `parent_dev_hdl`, no longer at
  `parent_port_num == 0`. A device behind an external hub uses its parent's
  root port.
- Only one device can sit at address 0 (USBH and the enumeration look it up
  by address), so a connection on a root port while another device is at
  address 0 waits: `hub_root_enum_done()` (called by `usb_host.c` when an
  enumeration completes or is canceled) and every pass of `hub_process()`
  take it up once address 0 is free.
- `usbh.c`: `usbh_dev_get_port_hdl()` (a child's root port) and
  `usbh_devs_addr_in_use()` (address 0 taken, in any state: a device being
  enumerated is locked, and `usbh_devs_open()` answers "not allowed").
- The PHYs: `usb_host_install()` still sets up only one; with both
  controllers P4OS passes `skip_phy_setup` and sets up the two itself
  (`components/aos_hal/aos_usb_p4.c`).

## Updating

To move to a newer ESP-IDF: copy its `components/usb` over this one (without
`test_apps`, `host_test` and `maintainers.md`), commit that alone, and
re-apply the changes above (`git show` of the commits after the copy).
