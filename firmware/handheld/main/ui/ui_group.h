/*
 * ui_group.h - make, change, and remove a group from the handheld (D52).
 * Reads and edits groups through hh_service.h only (D27).
 */
#pragma once

#include <stdint.h>

/* Opens the Groups screen, reached from its own launcher tile: every group, and + for a new one. */
void ui_group_open_list(void);

/* Opens the group editor: id 0 makes a new group, any other id edits that group. Save, Remove,
 * and back all return to the Groups screen. */
void ui_group_open(uint16_t id);
