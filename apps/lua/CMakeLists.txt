cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lua)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# Builds lua.so instead of a whole firmware.
#
# project_so() gathers the sources with file(GLOB_RECURSE "*.c") over the
# component's directory, so EVERYTHING under main/ -main/lua/ included- goes
# into the .so without being listed. That is why main/lua/ cannot have lua.c
# or luac.c: they bring main() and are the desktop executables.
#
# 32 bits: 'int' integers and 'float' numbers instead of long long and
# double. On the watch this was the condition for building at all (the S3's
# FPU is single precision and the table had no double helpers: 45 symbols
# missing without it). The P4 is the same kind of core -single-precision FPU,
# double done in software by libgcc's __adddf3 and friends- so the reason is
# now speed instead of symbols: every number a script touches stays in the
# FPU. It is also what keeps the scripts behaving exactly as on the watch.
#
# Passed by -D and not by editing luaconf.h so the lua/ tree stays lua.org's
# except for the #ifndef that makes this -D possible (lua/VENDOR.md).
idf_build_set_property(COMPILE_DEFINITIONS "LUA_32BITS=1" APPEND)

# LUAI_MAXCCALLS is Lua's guard against its own recursion in C: the parser's
# nesting and every Lua -> C -> Lua level (pcall, a sort comparator, gsub with
# a function). Lua's default is 200.
#
# On the watch the scripts ran in the LVGL task and the guard had to come
# down to 40: 68 nested parentheses overflowed its 16 KB stack and rebooted
# the watch. On the P4 that task has 12 KB, and the frames are bigger:
# measured with the board's GCC (-fstack-usage, -Os), a nested parenthesis
# costs ~144 bytes of C stack in the parser and a nested pcall ~700, of
# which 352 are luaD_rawrunprotected's jmp_buf. Forty nested pcalls alone
# would be 28 KB. No number fits that stack and still lets a script call
# pcall.
#
# So here the interpreter has a stack of its own: every call into Lua runs on
# the app's thread (lua_app.c, LUA_STACK), 96 KB in PSRAM, and the guard is
# sized to it. 100 levels are ~70 KB in the worst case -a hundred pcalls
# inside each other- and ~18 KB of parser nesting, so a script that goes
# deeper gets "C stack overflow" as an ordinary Lua error and the board goes
# on. It still does not limit a script's own recursion: a Lua function
# calling itself stays inside the VM's loop and stacks no C frames.
idf_build_set_property(COMPILE_DEFINITIONS "LUAI_MAXCCALLS=100" APPEND)

include(elf_loader)
project_so(lua)
