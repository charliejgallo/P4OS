# Lua 5.4.8 for Tortuga

A copy of `apps/lua/main/lua` (see `VENDOR.md`: the files as lua.org ships
them, without `lua.c`, `luac.c`, `liolib.c`, `loslib.c`, `loadlib.c`,
`ldblib.c` and `linit.c`). It lives outside `main/` on purpose: the board's
build compiles every `.c` under `main/`, and the simulator links every app
together, so these files are not compiled on their own. `main/tg_lua.c`
includes them as one unit with every function static, the way Lua's
`onelua.c` does.
