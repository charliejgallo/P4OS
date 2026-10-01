cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_so)
# Builds hello_so.so (not a firmware): copy it to /sdcard/bench/ and run
#   elf so bench/hello_so.so
include(elf_loader)
project_so(hello_so)
