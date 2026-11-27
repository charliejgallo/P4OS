/*
 * P4OS - Vision, the engine: the end of the constructor table (board only).
 *
 * The loader does not run a module's .init_array, so ve_engine.cpp runs it
 * itself, between its start marker (in .init_array.00000, which the linker
 * sorts first) and this one, in plain .init_array from the last object of
 * the link (sources.cmake puts it there), so after every other constructor.
 * The same as CAS's engine/cas_end.c.
 */
typedef void (*ve_ctor_t)(void);

__attribute__((used, section(".init_array"))) const ve_ctor_t ve_ctor_end[1] = {0};
