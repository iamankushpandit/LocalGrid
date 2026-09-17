/*
 * lg_bsp_i2c.h - the I2C bus, shared inside lg_bsp only.
 *
 * The touch driver opens the board's bus; the audio driver needs the same handle to look for a
 * codec. This header is deliberately not in include/: lg_draw includes lg_bsp_touch.h, and a
 * public declaration would pull driver/i2c_master.h across the layer boundary (D27).
 */
#pragma once

#include "driver/i2c_master.h"

/* The bus the touch controller opened, or NULL on a board with no I2C touch. */
i2c_master_bus_handle_t lg_bsp_touch_i2c_bus(void);
