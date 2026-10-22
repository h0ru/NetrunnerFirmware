/**
 * @file board.h
 * Board selection header — includes the active board definition.
 *
 * This port targets only the LilyGo T-Embed CC1101.
 * The build system defines BOARD_INCLUDE to the correct header path.
 */

#pragma once

#if defined(BOARD_INCLUDE)
/* CMake sets BOARD_INCLUDE to e.g. "board_lilygo_t_embed_cc1101.h" */
#include BOARD_INCLUDE
#else
/* Default board */
#include "board_lilygo_t_embed_cc1101.h"
#endif

/* Sanity check: every board must define these */
#ifndef BOARD_NAME
#error "Board header must define BOARD_NAME"
#endif
#ifndef BOARD_ID
#define BOARD_ID "unknown"
#endif
#ifndef BOARD_PIN_LCD_MOSI
#error "Board header must define LCD pins"
#endif
