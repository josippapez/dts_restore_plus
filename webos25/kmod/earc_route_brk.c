// SPDX-License-Identifier: GPL-2.0-only
/*
 * earc_route for kernels hooked with plain BRK breakpoints and a target hook
 * table. build.py picks EARC_PLATFORM and EARC_TARGET; everything else is
 * the shared core. Experimental: not shipped.
 */
#define EARC_NAME "earc_route"
#include EARC_PLATFORM
#include EARC_TARGET
#include "core/core.h"
