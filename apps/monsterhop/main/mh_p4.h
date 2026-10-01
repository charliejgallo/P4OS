/*
 * MONSTER HOP on P4OS - the build's switches
 *
 * Every file of the game includes this first (through mh_gfx.h). They are
 * here and not in CMake because the simulator compiles every app of
 * sim/CMakeLists.txt into one program with the same flags: a -D for this
 * game would reach the others.
 *
 *   MH_P4            what only the P4 does: a screen that turns, touch
 *                    only, the frame blitted 1:1 or scaled by the HAL
 *   MH_VIEW_RUNTIME  the frame's size (mh_view_w/h) and the art's scale
 *                    (mh_px) are variables, set before a level loads
 */
#pragma once

#ifndef MH_P4
#define MH_P4 1
#endif
#ifndef MH_VIEW_RUNTIME
#define MH_VIEW_RUNTIME 1
#endif
