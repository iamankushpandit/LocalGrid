/*
 * ui_snapshot.h - the one status copy and the one message list every screen reads.
 *
 * All screens run on the drawing task, one callback at a time, so they can share a single
 * copy of each instead of keeping their own: nine static copies used to cost about 26 KB of
 * RAM on the Hosyond, which has none to spare.
 *
 * Each call refreshes the copy from the service and returns it. A caller that holds the
 * pointer and then calls something that refreshes it again sees newer data under the same
 * pointer, which is still one consistent snapshot; callers that need two different moments
 * must copy what they compare (the screens compare versions and signatures, not whole copies).
 * Drawing task only.
 */
#pragma once

#include <stddef.h>

#include "hh_service.h"

const hh_status_t *ui_status(void);

/* Newest first; *count receives how many there are. */
const hh_message_t *ui_messages(size_t *count);
