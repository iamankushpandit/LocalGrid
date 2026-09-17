/*
 * lg_bsp_settings.h - small stored preferences, kept where the platform belongs.
 *
 * The screens own how a handheld looks and behaves; they do not own flash. lg_draw reaches
 * hardware and platform services only through lg_bsp (D27), and `tools/check_layers.py`
 * enforces it: a `#include "nvs.h"` in lg_draw is rejected before the build starts, which is how
 * the screen saver's own setting first tried to store itself.
 *
 * One namespace, `lgui`, shared with the touch calibration and the audio volume, so every
 * remembered preference sits in one place and a factory reset clears them together.
 *
 * Only the shapes actually needed are here. A preference that wants a string or a struct can
 * have its own call when something needs one; guessing at an interface nothing uses yet is how
 * a settings layer turns into a database.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Reads a stored flag. Returns `fallback` when the key has never been written, which is the
 * normal case on a handheld nobody has changed, and also when the store cannot be opened --
 * a missing preference should leave the device usable rather than refusing to start.
 */
bool lg_bsp_setting_get_bool(const char *key, bool fallback);

/* Writes a flag and commits it, so it survives a reboot and a reflash (D22 keeps settings). */
esp_err_t lg_bsp_setting_set_bool(const char *key, bool value);
