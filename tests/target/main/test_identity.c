/*
 * Device identity checks: a board proves the identity it stores was minted for it.
 *
 * Two boards answering as the same device broke a bench run - the same ID, the same static IP,
 * one handheld apparently in two places - because the provisioning tool writes whatever ID the
 * device map holds onto whatever board is on the port. The board now recomputes its own ID from
 * its MAC and the stored mint nonce, and a clone is caught at boot instead of joining the grid.
 *
 * These use a fixed MAC rather than the board's own, so the vectors hold on every bench board and
 * no hardware address is ever printed (D21). The expected IDs were produced by tools/flash.py
 * mint_id(), which is the only thing this has to agree with.
 */
#include <string.h>

#include "lg_identity.h"
#include "lg_test.h"

/* A MAC that belongs to no board here; the digit changed in MAC_OTHER is the last one. */
static const uint8_t MAC_THIS[6]  = { 0x24, 0x6f, 0x28, 0x1a, 0x2b, 0x3c };
static const uint8_t MAC_OTHER[6] = { 0x24, 0x6f, 0x28, 0x1a, 0x2b, 0x3d };

#define NONCE_H   "0123456789abcdef"
#define CREATED_H 1789494151u
#define ID_H      "LG-H-E28-YQPDRVEZJH"

#define NONCE_N   "a1b2c3d4e5f60718"
#define CREATED_N 1789492913u
#define ID_N      "LG-N-ELG-1J51BV77XV"

static lg_identity_t handheld(void)
{
    lg_identity_t id;
    memset(&id, 0, sizeof(id));
    id.present = true;
    snprintf(id.id, sizeof(id.id), "%s", ID_H);
    snprintf(id.role, sizeof(id.role), "H");
    snprintf(id.board, sizeof(id.board), "E28");
    id.created = CREATED_H;
    snprintf(id.nonce, sizeof(id.nonce), NONCE_H);
    id.has_device = true;
    id.device_index = 4;
    return id;
}

void test_identity(void)
{
    char out[LG_IDENTITY_ID_MAX];

    /* 1. The recompute agrees with the tool for a known (role, board, MAC, created, nonce). */
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H, NONCE_H, out, sizeof(out)), ESP_OK);
    CHECK(strcmp(out, ID_H) == 0);
    CHECK_EQ(lg_identity_compute_id("N", "ELG", MAC_THIS, CREATED_N, NONCE_N, out, sizeof(out)), ESP_OK);
    CHECK(strcmp(out, ID_N) == 0);

    /* It is a function of its inputs: the same inputs twice give the same ID. */
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H, NONCE_H, out, sizeof(out)), ESP_OK);
    CHECK(strcmp(out, ID_H) == 0);

    /* 2. Another board's MAC gives another ID; so do another mint time and another nonce. */
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_OTHER, CREATED_H, NONCE_H, out, sizeof(out)), ESP_OK);
    CHECK(strcmp(out, ID_H) != 0);
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H + 1u, NONCE_H, out, sizeof(out)), ESP_OK);
    CHECK(strcmp(out, ID_H) != 0);
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H, "00000000deadbeef", out, sizeof(out)), ESP_OK);
    CHECK(strcmp(out, ID_H) != 0);

    /* A malformed or missing nonce is not silently hashed as text. */
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H, "", out, sizeof(out)), ESP_ERR_INVALID_ARG);
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H, "NOTHEX", out, sizeof(out)),
             ESP_ERR_INVALID_ARG);
    CHECK_EQ(lg_identity_compute_id("H", "E28", MAC_THIS, CREATED_H, NONCE_H, out, 8), ESP_ERR_INVALID_ARG);

    /* 3. The board it was minted for accepts it; any other board reports a mismatch. */
    lg_identity_t id = handheld();
    CHECK_EQ(lg_identity_check(&id, MAC_THIS), LG_IDENTITY_OK);
    CHECK_EQ(lg_identity_check(&id, MAC_OTHER), LG_IDENTITY_MISMATCH);

    /* 4. No stored nonce: every board provisioned before the nonce was kept. Cannot check, and
     *    cannot be reported as a mismatch, or the whole bench would refuse to start. */
    lg_identity_t old_board = handheld();
    old_board.nonce[0] = '\0';
    CHECK_EQ(lg_identity_check(&old_board, MAC_THIS), LG_IDENTITY_UNCHECKED);
    CHECK_EQ(lg_identity_check(&old_board, MAC_OTHER), LG_IDENTITY_UNCHECKED);

    /* Nothing provisioned at all is equally unchecked, never a mismatch. */
    lg_identity_t blank;
    memset(&blank, 0, sizeof(blank));
    CHECK_EQ(lg_identity_check(&blank, MAC_THIS), LG_IDENTITY_UNCHECKED);
    CHECK_EQ(lg_identity_check(NULL, MAC_THIS), LG_IDENTITY_UNCHECKED);

    /* 5. A tampered stored ID is a mismatch even on the board it was minted for: one character
     *    of the tail changed, which is what a hand-edited device map looks like. */
    lg_identity_t tampered = handheld();
    tampered.id[strlen(tampered.id) - 1] = (char)(tampered.id[strlen(tampered.id) - 1] == 'H' ? 'J' : 'H');
    CHECK_EQ(lg_identity_check(&tampered, MAC_THIS), LG_IDENTITY_MISMATCH);

    /* Fields that are part of the hash: a swapped role or board no longer matches the stored ID. */
    lg_identity_t wrong_board = handheld();
    snprintf(wrong_board.board, sizeof(wrong_board.board), "CYD");
    CHECK_EQ(lg_identity_check(&wrong_board, MAC_THIS), LG_IDENTITY_MISMATCH);
    lg_identity_t wrong_created = handheld();
    wrong_created.created = CREATED_H + 60u;
    CHECK_EQ(lg_identity_check(&wrong_created, MAC_THIS), LG_IDENTITY_MISMATCH);

    /* 6. The words that reach a log or a screen are the plain ones, for every result. */
    CHECK(strcmp(lg_identity_check_text(LG_IDENTITY_OK), "this board's own") == 0);
    CHECK(strcmp(lg_identity_check_text(LG_IDENTITY_MISMATCH), "another board's") == 0);
    CHECK(strcmp(lg_identity_check_text(LG_IDENTITY_UNCHECKED), "not checked") == 0);
}
