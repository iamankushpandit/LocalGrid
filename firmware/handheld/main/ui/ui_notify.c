/*
 * Message notifications (P6).
 *
 * A timer watches the service's message list. When a message arrives that this handheld
 * did not send, and its conversation is not the one on screen, a banner names the sender
 * and shows a preview; tapping it opens that conversation. Unread counts are kept per
 * conversation and cleared when it is opened.
 *
 * Counts live in RAM with the messages themselves, so a reboot clears both.
 */
#include "ui_notify.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_envelope.h"
#include "lg_theme.h"
#include "lg_ui_widgets.h"
#include "ui_alert.h"
#include "ui_chat.h"

static const char *TAG = "UI";

#define WATCH_MS      400
#define CONVS_TRACKED (LG_MAX_DEVICES + LG_MAX_GROUPS + 1)

typedef struct {
    bool     used;
    uint8_t  scope;
    uint32_t target;
    uint32_t unread;
} tracked_t;

static struct {
    tracked_t convs[CONVS_TRACKED];
    uint32_t  seen_id;        /* highest message id already considered */
    uint8_t   pending_scope;  /* conversation the banner opens */
    uint32_t  pending_target;
    char      pending_title[HH_NAME_MAX];
} s_notify;

/* A message's conversation: broadcast, a group, or the other handheld in a 1:1. */
static void conversation_of(const hh_message_t *m, uint8_t *scope, uint32_t *target)
{
    *scope = m->scope;
    *target = m->scope == LG_SCOPE_DIRECT ? (m->mine ? m->target : m->author) : m->target;
}

static tracked_t *slot_for(uint8_t scope, uint32_t target, bool create)
{
    for (size_t i = 0; i < CONVS_TRACKED; i++) {
        tracked_t *c = &s_notify.convs[i];
        if (c->used && c->scope == scope && c->target == target) {
            return c;
        }
    }
    if (!create) {
        return NULL;
    }
    for (size_t i = 0; i < CONVS_TRACKED; i++) {
        tracked_t *c = &s_notify.convs[i];
        if (!c->used) {
            c->used = true;
            c->scope = scope;
            c->target = target;
            c->unread = 0;
            return c;
        }
    }
    return NULL;
}

uint32_t ui_notify_unread(uint8_t scope, uint32_t target)
{
    const tracked_t *c = slot_for(scope, target, false);
    return c != NULL ? c->unread : 0;
}

uint32_t ui_notify_unread_total(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < CONVS_TRACKED; i++) {
        if (s_notify.convs[i].used) {
            total += s_notify.convs[i].unread;
        }
    }
    return total;
}

void ui_notify_mark_seen(uint8_t scope, uint32_t target)
{
    tracked_t *c = slot_for(scope, target, false);
    if (c != NULL) {
        c->unread = 0;
    }
}

static void on_banner_tapped(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "[UI] Banner tapped: opening %s", s_notify.pending_title);
    lg_ui_toast_hide();
    ui_chat_open_conversation(s_notify.pending_scope, s_notify.pending_target, s_notify.pending_title);
}

static const char *sender_name(const hh_status_t *st, uint32_t device)
{
    for (uint8_t i = 0; i < st->n_people; i++) {
        if (st->people[i].device == device) {
            return st->people[i].name;
        }
    }
    return "A handheld";
}

static void conversation_title(const hh_status_t *st, uint8_t scope, uint32_t target, const hh_message_t *m,
                               char *out, size_t cap)
{
    if (scope == LG_SCOPE_BROADCAST) {
        snprintf(out, cap, "Everyone");
        return;
    }
    if (scope == LG_SCOPE_GROUP) {
        for (uint8_t i = 0; i < st->n_groups; i++) {
            if (st->groups[i].id == (uint16_t)target) {
                snprintf(out, cap, "%s", st->groups[i].name);
                return;
            }
        }
        snprintf(out, cap, "Group %" PRIu32, target);
        return;
    }
    snprintf(out, cap, "%s", sender_name(st, m->author));
}

static void watch(lv_timer_t *timer)
{
    (void)timer;
    static hh_message_t msgs[HH_MESSAGES];
    static hh_status_t st;
    size_t n = hh_service_messages(msgs, HH_MESSAGES);
    if (n == 0) {
        return;
    }
    hh_service_status(&st);

    /* The list arrives newest first; walk it oldest first so counts follow arrival order. */
    const hh_message_t *newest_unseen = NULL;
    uint8_t newest_scope = 0;
    uint32_t newest_target = 0;
    for (size_t i = n; i > 0; i--) {
        const hh_message_t *m = &msgs[i - 1];
        if (m->id <= s_notify.seen_id || m->mine) {
            continue;
        }
        uint8_t scope;
        uint32_t target;
        conversation_of(m, &scope, &target);
        if (ui_chat_conversation_open(scope, target)) {
            continue;   /* the owner is reading it right now */
        }
        tracked_t *c = slot_for(scope, target, true);
        if (c != NULL) {
            c->unread++;
        }
        newest_unseen = m;
        newest_scope = scope;
        newest_target = target;
    }
    s_notify.seen_id = msgs[0].id;   /* newest */
    if (newest_unseen == NULL) {
        return;
    }

    s_notify.pending_scope = newest_scope;
    s_notify.pending_target = newest_target;
    conversation_title(&st, newest_scope, newest_target, newest_unseen, s_notify.pending_title,
                       sizeof(s_notify.pending_title));
    char text[HH_TEXT_MAX + 64];
    const char *who = sender_name(&st, newest_unseen->author);
    if (newest_scope == LG_SCOPE_DIRECT) {
        snprintf(text, sizeof(text), "%s%s: %s", who, newest_unseen->urgent ? " (urgent)" : "", newest_unseen->text);
    } else {
        snprintf(text, sizeof(text), "%s in %s%s: %s", who, s_notify.pending_title,
                 newest_unseen->urgent ? " (urgent)" : "", newest_unseen->text);
    }
    /*
     * Three levels of insistence. A message for you gets a banner and a bell: look when you
     * look. A broadcast is for everybody, so it flashes the screen and chimes -- meant to be
     * caught across a tent. An urgent broadcast takes the screen until somebody acknowledges
     * it, and sounds even on a handheld that has been silenced.
     */
    if (newest_scope == LG_SCOPE_BROADCAST) {
        ui_alert_show(newest_unseen->urgent ? UI_ALERT_EMERGENCY : UI_ALERT_ANNOUNCEMENT,
                      who, newest_unseen->text);   /* the alert plays its own cue */
    } else {
        lg_bsp_audio_cue(LG_CUE_RECEIVED);
        lg_ui_toast(text, on_banner_tapped);
    }
    ESP_LOGI(TAG, "[UI] Notified: %s", text);
}

void ui_notify_start(void)
{
    lv_timer_create(watch, WATCH_MS, NULL);
}
