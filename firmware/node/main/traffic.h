/*
 * traffic.h - packet flows, faults and AP performance for the BLE admin link (D70).
 *
 * Counts and sizes only. Nothing here holds a message, a name, an address, or any audio: the
 * record says how many frames of each class passed, how many were dropped and why, how busy each
 * backbone link and the handheld side were, and how the AP itself is coping. The watcher does the
 * arithmetic and the wording (D49); the AP only keeps bytes.
 *
 * The hot path costs one add per frame (in lg_core, backbone.c and sessions.c). The bucket ring
 * (TRAFFIC_BUCKETS x TRAFFIC_BUCKET_MS, so a one-minute and a five-minute rate) rolls once a
 * second on the core task, which also rebuilds the packed record. The BLE link task copies the
 * finished bytes out under a mutex and never reads live state.
 *
 * Layout: docs/ble-link.md, "Traffic and performance".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TRAFFIC_BUCKETS    10u
#define TRAFFIC_BUCKET_MS  30000u
#define TRAFFIC_LAYOUT     1u

#define LORA_SECTION_LEN   64u

/*
 * Header 24 + classes 120 + faults 40 + sessions 28 + performance 40 + buckets 40 + links 8 x 28,
 * and after all of them the LoRa section of D71.
 *
 * The layout number stays 1. The LoRa bytes are APPENDED after the variable-length link entries,
 * which a reader already finds by the link count in the header, so a decoder written before D71
 * reads every field it knows exactly where it did and ignores what follows (tools/grid_watch.py
 * says so in as many words). Never renumber or move a field here; only add at the end.
 */
#define TRAFFIC_RECORD_MAX (516u + LORA_SECTION_LEN)

esp_err_t traffic_init(void);

/* The tasks whose stack headroom the record reports. Either may be NULL. */
void traffic_set_tasks(TaskHandle_t core, TaskHandle_t link);

/* Core task, once per pass: how long the previous pass took. */
void traffic_loop_pass(uint32_t us);

/* Core task, once a second: rolls the bucket ring and rebuilds the record. */
void traffic_publish(uint32_t now_ms);

/*
 * Copies the newest record into out (at least TRAFFIC_RECORD_MAX bytes) and returns its length,
 * or 0 if it is not ready or the lock was busy for wait_ms. Any task.
 */
size_t traffic_record(uint8_t *out, size_t cap, uint32_t wait_ms);
