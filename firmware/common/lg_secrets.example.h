/*
 * Template for lg_secrets.h. Do not put real keys here.
 * Generate the real file with:  python tools/gen_secrets.py
 * lg_secrets.h is gitignored; every board in one grid must share it.
 */
#pragma once

#define LG_SECRET_WIFI_PASSPHRASE "change-me-20-chars!!"

#define LG_SECRET_BACKBONE_KEY { 0 }

/* HKDF-SHA256(backbone key, "lg-disc"), 4 bytes. Handhelds use this and never the backbone key. */
#define LG_SECRET_DISCRIMINATOR { 0, 0, 0, 0 }

/* D76: the LoRa key APs and handhelds share. tools/gen_secrets.py derives it from the backbone
   key, so it belongs to this grid alone and cannot be worked back to the backbone key, which
   handhelds never hold. */
#define LG_SECRET_LORA_KEY { 0 }
