/*
 * hh_console.h - serial console for the handheld (decision D28).
 * Answers `id` for tools/flash.py, plus status and configuration commands.
 */
#pragma once

#include "esp_err.h"
#include "lg_identity.h"

/* Starts the REPL on this board's console port. identity must outlive the console. */
esp_err_t hh_console_start(const lg_identity_t *identity);
