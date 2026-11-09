# Equipos

The bench's own Modbus devices as panels. Id `aos.equipos`. Taken out of
the firmware's old Modbus app (until 0.24) when that one became a generic
Modbus tester (`apps/modbus`); what it does did not change.

- **Riden:** an RD60xx power supply over Modbus RTU (its TTL port at
  115200, unit 1): the output live and big (CV, CC, OVP/OCP), the setpoints
  (each write read back, because the supply rejects in silence what it does
  not like), the ceiling the input sets, the presets M1-M9, input voltage,
  temperature, Ah, Wh, keypad lock, and the power over the last minute. The
  register map is the one measured on the real unit (`riden-psu/README.md`).
- **Gateway:** a DOMCOM gateway over Modbus TCP: its sensors, relays,
  auxiliary modules, dimmers and magnetic sensors (its README's map).

The talking is the firmware's Modbus master (`aos_modbus.c`). The Riden's
link is kept in the preferences `mb_rd_port`, `mb_rd_baud` and
`mb_rd_unit`, the same ones the firmware's Riden service reads for the
Banco's Fuente and its logger (`aos_riden.c`): changing it here moves the
Banco's too. Only one of the two holds the port at a time.

In the simulator `tools/fake_modbus.py --rtu --tcp 1502` plays both: a
Riden on a pty (`P4_SIM_UART_A=<it>`) and a gateway on 127.0.0.1:1502.
