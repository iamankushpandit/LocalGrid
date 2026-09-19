/*
 * lg_bsp_board.h - telling apart two boards sold under one name (board support layer, D66).
 */
#pragma once

#include "lg_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The profile to drive this board with. Returns `named` unless the profile has a sibling and an
 * I2C probe says the board is the sibling: a part only the sibling has answers, or a part only
 * `named` has does not. Logs what it looked for and what it decided, with the tag [BSP]. Call once
 * at boot, before anything claims the probe pins. NULL in, NULL out.
 */
const lg_board_t *lg_bsp_board_resolve(const lg_board_t *named);

#ifdef __cplusplus
}
#endif
