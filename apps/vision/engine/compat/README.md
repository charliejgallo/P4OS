# The simulator's stand-ins for ESP-IDF

esp-dl includes a dozen ESP-IDF headers. In the simulator (and in the
check of the models on the Mac, `tools/ve_check.sh`) these take their place:
the logging prints to stderr, the heap is malloc, there is one core and no
cache to manage. Only what esp-dl uses is here. The board build never sees
this folder: it has the real headers, and what the firmware does not export
is in `../ve_rt.cpp`.
