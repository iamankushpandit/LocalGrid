/*
 * The self-running demonstration: see hh_demo.h.
 *
 * One task, which sleeps between steps. It only ever asks hh_service and hh_voice to do the same
 * things a person would, so nothing here is a special path that works when the real one does not:
 * if the demo shows a message delivered, a message really was delivered.
 */
#include "hh_demo.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hh_voice.h"
#include "lg_envelope.h"
#include "lg_types.h"

static const char *TAG = "DEMO";

#define TASK_STACK    4096
#define TASK_PRIORITY 2          /* below voice (4) and drawing (3): a demo never delays real work */
#define SETTLE_MS     2500       /* between steps, so a watcher can see each one happen */
#define DELIVER_MS    8000       /* how long a 1:1 message is given to come back delivered */
#define VOICE_MS      3000       /* a generated talk: long enough to hear, short enough to sit through */
#define ROAM_WAIT_MS  45000      /* D54's brakes mean rejoining is not instant */

static struct {
    portMUX_TYPE     mux;
    hh_demo_state_t  st;         /* guarded by mux */
    TaskHandle_t     task;
    volatile bool    run_req;
    volatile bool    stop_req;
    volatile uint8_t mode_req;
} d = { .mux = portMUX_INITIALIZER_UNLOCKED };

/*
 * The demo task's big structures live here, not on its stack. hh_status_t carries the people, the
 * users and the groups tables and runs to well over a kilobyte; two of them in one call chain
 * overflowed a 3.5 KB stack and restarted the board (bench, 2026-09-21). Only the demo task
 * touches these, and it runs one step at a time, so sharing them is safe.
 */
static hh_status_t s_status;
static hh_message_t s_msg;

/* ---- state, for the screens (D23) ---- */

static void publish(hh_demo_step_t step, uint8_t step_n, const char *note)
{
    taskENTER_CRITICAL(&d.mux);
    d.st.step = step;
    d.st.step_n = step_n;
    if (note != NULL) {
        snprintf(d.st.note, sizeof(d.st.note), "%s", note);
    }
    d.st.version++;
    taskEXIT_CRITICAL(&d.mux);
    ESP_LOGI(TAG, "[DEMO] step %u/%u: %s", step_n, d.st.steps, note ? note : "");
}

static void finished(uint8_t step_n, bool ok, const char *note)
{
    taskENTER_CRITICAL(&d.mux);
    if (ok) {
        d.st.done++;
    } else {
        d.st.skipped++;
    }
    if (note != NULL) {
        snprintf(d.st.note, sizeof(d.st.note), "%s", note);
    }
    d.st.version++;
    taskEXIT_CRITICAL(&d.mux);
    ESP_LOGI(TAG, "[DEMO] step %u %s: %s", step_n, ok ? "done" : "skipped", note ? note : "");
}

void hh_demo_state(hh_demo_state_t *out)
{
    taskENTER_CRITICAL(&d.mux);
    *out = d.st;
    taskEXIT_CRITICAL(&d.mux);
}

/* ---- what the grid can show, worked out when the run starts ---- */

/*
 * Everyone the grid says is online, with what their device reports it can do. Read once per run:
 * a demo that re-read it between steps could change its mind halfway through and be impossible to
 * follow. Anyone who arrives mid-run joins the next one.
 */
typedef struct {
    uint8_t      n;
    hh_person_t  who[LG_MAX_DEVICES];
    uint16_t     group;          /* a group this handheld belongs to, 0 if none */
    bool         any_speaker;    /* somebody can play a talk, so sending one is worth doing */
    bool         any_gps;        /* somebody can report a position */
    bool         can_talk;       /* this handheld can put a generated talk on the air */
} cast_t;

static void read_cast(cast_t *c)
{
    hh_service_status(&s_status);
    memset(c, 0, sizeof(*c));
    for (uint8_t i = 0; i < s_status.n_people && c->n < LG_MAX_DEVICES; i++) {
        const hh_person_t *person = &s_status.people[i];
        if (!person->online) {
            continue;
        }
        c->who[c->n++] = *person;
        if ((person->caps & LG_CAP_SPEAKER) || person->caps == 0) {
            c->any_speaker = true;   /* 0 is "nobody has said", so try rather than skip */
        }
        if (person->caps & LG_CAP_GPS) {
            c->any_gps = true;
        }
    }
    for (uint8_t i = 0; i < s_status.n_groups; i++) {
        if (s_status.groups[i].member) {
            c->group = s_status.groups[i].id;
            break;
        }
    }
    hh_voice_state_t vs;
    hh_voice_state(&vs);
    /* Either a microphone or a speaker is enough: a generated talk needs the audio path, not a
     * person. A board with neither (the alert unit, D66) simply skips the voice step. */
    c->can_talk = vs.can_talk || vs.can_hear;
}

/* The person a 1:1 step should use: whoever has the most capabilities, so the demo shows the most
 * it can. With one other handheld in the grid, that is the one. */
static const hh_person_t *best_peer(const cast_t *c, uint16_t want)
{
    /* Capabilities of 0 mean nobody has said, not that the device can do nothing: an AP running a
     * build from before presence carried them reports 0 for everyone it speaks for. Filtering on a
     * capability nobody has reported would silently drop every step that asks for one, so when the
     * grid has told us nothing at all, ask anyway and let the attempt succeed or fail honestly. */
    bool known = false;
    for (uint8_t i = 0; i < c->n; i++) {
        if (c->who[i].caps != 0) {
            known = true;
            break;
        }
    }
    const hh_person_t *best = NULL;
    for (uint8_t i = 0; i < c->n; i++) {
        if (want != 0 && known && !(c->who[i].caps & want)) {
            continue;
        }
        if (best == NULL || __builtin_popcount(c->who[i].caps) > __builtin_popcount(best->caps)) {
            best = &c->who[i];
        }
    }
    return best;
}

/* ---- waiting ---- */

static bool sleep_ms(uint32_t ms)
{
    const uint32_t slice = 250;
    for (uint32_t waited = 0; waited < ms; waited += slice) {
        if (d.stop_req) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(slice));
    }
    return !d.stop_req;
}

/* True once our newest sent message reached the state we hoped for, or the wait ran out. */
static bool wait_delivered(uint32_t ms, bool *delivered)
{
    *delivered = false;
    for (uint32_t waited = 0; waited < ms; waited += 250) {
        if (d.stop_req) {
            return false;
        }
        if (hh_service_message(0, &s_msg) && s_msg.mine &&
            (s_msg.state == HH_MSG_DELIVERED || s_msg.state == HH_MSG_READ)) {
            *delivered = true;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return true;
}

/* ---- the steps ---- */

static void step_direct(const cast_t *c, uint8_t n)
{
    const hh_person_t *peer = best_peer(c, 0);
    if (peer == NULL) {
        finished(n, false, "nobody else is online for a 1:1 message");
        return;
    }
    char text[HH_TEXT_MAX + 1];
    snprintf(text, sizeof(text), "Demo: a message for you alone, %s.", peer->name);
    publish(HH_DEMO_DIRECT, n, text);
    if (hh_service_send(LG_SCOPE_DIRECT, peer->device, false, text) != ESP_OK) {
        finished(n, false, "the handheld would not send a 1:1 message");
        return;
    }
    bool ok = false;
    if (!wait_delivered(DELIVER_MS, &ok)) {
        return;
    }
    char note[HH_DEMO_NOTE_MAX];
    snprintf(note, sizeof(note), ok ? "delivered to %s, end to end encrypted" : "sent to %s, not confirmed yet",
             peer->name);
    finished(n, ok, note);
}

static void step_group(const cast_t *c, uint8_t n)
{
    if (c->group == 0) {
        finished(n, false, "this handheld is in no group, so no group message");
        return;
    }
    publish(HH_DEMO_GROUP, n, "a message to everyone in the group");
    if (hh_service_send(LG_SCOPE_GROUP, c->group, false,
                        "Demo: this one goes to the whole group at once.") != ESP_OK) {
        finished(n, false, "the group message would not go");
        return;
    }
    (void)sleep_ms(SETTLE_MS);
    char note[HH_DEMO_NOTE_MAX];
    if (hh_service_message(0, &s_msg) && s_msg.mine) {
        snprintf(note, sizeof(note), "%u in the group have it", s_msg.delivered_count);
    } else {
        snprintf(note, sizeof(note), "sent to the group");
    }
    finished(n, true, note);
}

static void step_voice(const cast_t *c, uint8_t n)
{
    if (!c->can_talk) {
        finished(n, false, "this board has no audio, so it cannot talk");
        return;
    }
    const hh_person_t *peer = best_peer(c, LG_CAP_SPEAKER);
    if (peer == NULL) {
        finished(n, false, "nobody online has a speaker to hear a talk");
        return;
    }
    char note[HH_DEMO_NOTE_MAX];
    snprintf(note, sizeof(note), "talking to %s: a generated tone", peer->name);
    publish(HH_DEMO_VOICE, n, note);
    esp_err_t err = hh_voice_tone_start(LG_SCOPE_DIRECT, peer->device, VOICE_MS);
    if (err != ESP_OK) {
        finished(n, false, err == ESP_ERR_INVALID_STATE ? "somebody else is talking" : "the talk would not start");
        return;
    }
    (void)sleep_ms(VOICE_MS + SETTLE_MS);
    snprintf(note, sizeof(note), "%u ms of voice went to %s", (unsigned)VOICE_MS, peer->name);
    finished(n, true, note);
}

static void step_announce(uint8_t n)
{
    publish(HH_DEMO_ANNOUNCE, n, "an announcement everybody sees");
    esp_err_t err = hh_service_send(LG_SCOPE_BROADCAST, LG_TARGET_ALL, false,
                                    "Demo: an announcement reaches every handheld in the grid.");
    if (err != ESP_OK) {
        /* D56: the admin page decides which handhelds may announce, so a refusal here is the grid
           working as configured, not a fault. Say which it was. */
        finished(n, false, "this handheld is not allowed to announce (D56)");
        return;
    }
    (void)sleep_ms(SETTLE_MS);
    finished(n, true, "the announcement went to the whole grid");
}

static void step_urgent(uint8_t n)
{
    publish(HH_DEMO_URGENT, n, "an urgent call: it sounds even on a muted handheld");
    if (hh_service_send(LG_SCOPE_BROADCAST, LG_TARGET_ALL, true,
                        "Demo: this is the urgent alert. It is only a demonstration.") != ESP_OK) {
        finished(n, false, "the urgent broadcast would not go");
        return;
    }
    if (!sleep_ms(SETTLE_MS * 2u)) {
        /* Stopped mid-step: the all clear still goes, because leaving an SOS standing on every
           screen in the grid is the one thing this demo must never do. */
        (void)hh_service_send_all_clear("Demo: all clear. The demonstration is over.");
        return;
    }
    publish(HH_DEMO_URGENT, n, "and the all clear that stands it down");
    (void)hh_service_send_all_clear("Demo: all clear. That was a demonstration.");
    (void)sleep_ms(SETTLE_MS);
    finished(n, true, "urgent alert raised and stood down");
}

static void step_position(const cast_t *c, uint8_t n)
{
    if (!c->any_gps) {
        hh_position_t mine;
        if (!hh_service_own_position(&mine)) {
            finished(n, false, "no device here has a position to show");
            return;
        }
    }
    publish(HH_DEMO_POSITION, n, "where everyone is, from the devices with a GPS");
    uint8_t with = 0;
    hh_position_t p;
    if (hh_service_own_position(&p)) {
        with++;
    }
    for (uint8_t i = 0; i < c->n; i++) {
        if (hh_service_position(c->who[i].device, &p)) {
            with++;
        }
    }
    char note[HH_DEMO_NOTE_MAX];
    snprintf(note, sizeof(note), "%u device(s) are reporting a position", with);
    finished(n, with > 0, note);
}

static void step_roam(uint8_t n)
{
    hh_service_status(&s_status);
    int was = s_status.node;   /* the one field needed later: s_status is reused below */
    char note[HH_DEMO_NOTE_MAX];
    snprintf(note, sizeof(note), "leaving AP %d to show the grid heal", was);
    publish(HH_DEMO_ROAM, n, note);
    hh_service_reconnect();
    /* Dropping the session is asynchronous, so the link is still ONLINE for a moment after asking.
     * Waiting for it to actually go before timing its return is the difference between measuring a
     * rejoin and measuring nothing: without this the step reported success in 0 s having never
     * left (bench, 2026-09-21). */
    bool left = false;
    for (uint32_t waited = 0; waited < 5000u; waited += 100) {
        if (d.stop_req) {
            return;
        }
        hh_service_status(&s_status);
        if (s_status.link != HH_LINK_ONLINE) {
            left = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!left) {
        finished(n, false, "the handheld never left its AP, so nothing was shown");
        return;
    }
    publish(HH_DEMO_ROAM, n, "off the grid: watching it find an AP again");
    for (uint32_t waited = 0; waited < ROAM_WAIT_MS; waited += 500) {
        if (d.stop_req) {
            return;
        }
        hh_service_status(&s_status);
        if (s_status.link == HH_LINK_ONLINE) {
            snprintf(note, sizeof(note), "back on AP %d after %" PRIu32 " s off the grid",
                     s_status.node, waited / 1000u);
            finished(n, true, note);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    finished(n, false, "did not rejoin an AP within the time allowed");
}

/* ---- the run ---- */

static void run(hh_demo_mode_t mode)
{
    cast_t c;
    publish(HH_DEMO_LOOKING, 0, "asking the grid who is here and what they can do");
    read_cast(&c);
    if (c.n == 0) {
        taskENTER_CRITICAL(&d.mux);
        snprintf(d.st.note, sizeof(d.st.note), "nobody else is online: a demo needs somebody to talk to");
        d.st.version++;
        taskEXIT_CRITICAL(&d.mux);
        ESP_LOGW(TAG, "[DEMO] nobody else is online; nothing to demonstrate");
        return;
    }
    /* The script is as long as the grid can support, so the count on screen is honest. */
    uint8_t steps = 4;                                  /* direct, announce, urgent, position */
    if (c.group != 0) {
        steps++;
    }
    if (c.can_talk && c.any_speaker) {
        steps++;
    }
    if (mode == HH_DEMO_RESILIENCE) {
        steps++;
    }
    taskENTER_CRITICAL(&d.mux);
    d.st.steps = steps;
    d.st.version++;
    taskEXIT_CRITICAL(&d.mux);
    ESP_LOGI(TAG, "[DEMO] %s run: %u people online, %u steps",
             mode == HH_DEMO_RESILIENCE ? "resilience" : "showcase", c.n, steps);

    uint8_t n = 0;
    step_direct(&c, ++n);
    if (!sleep_ms(SETTLE_MS)) {
        return;
    }
    if (c.group != 0) {
        step_group(&c, ++n);
        if (!sleep_ms(SETTLE_MS)) {
            return;
        }
    }
    if (c.can_talk && c.any_speaker) {
        step_voice(&c, ++n);
        if (!sleep_ms(SETTLE_MS)) {
            return;
        }
    }
    step_announce(++n);
    if (!sleep_ms(SETTLE_MS)) {
        return;
    }
    step_urgent(++n);
    if (!sleep_ms(SETTLE_MS)) {
        return;
    }
    step_position(&c, ++n);
    if (mode == HH_DEMO_RESILIENCE) {
        if (!sleep_ms(SETTLE_MS)) {
            return;
        }
        step_roam(++n);
    }
}

static void demo_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!d.run_req) {
            continue;
        }
        d.stop_req = false;
        taskENTER_CRITICAL(&d.mux);
        d.st.running = true;
        d.st.mode = (hh_demo_mode_t)d.mode_req;
        d.st.step = HH_DEMO_LOOKING;
        d.st.step_n = 0;
        d.st.steps = 0;
        d.st.done = 0;
        d.st.skipped = 0;
        d.st.note[0] = '\0';
        d.st.version++;
        taskEXIT_CRITICAL(&d.mux);

        run((hh_demo_mode_t)d.mode_req);

        char note[HH_DEMO_NOTE_MAX];
        taskENTER_CRITICAL(&d.mux);
        uint8_t done = d.st.done, skipped = d.st.skipped;
        taskEXIT_CRITICAL(&d.mux);
        snprintf(note, sizeof(note), d.stop_req ? "stopped after %u step(s)" : "%u shown, %u skipped",
                 d.stop_req ? done : done, skipped);
        taskENTER_CRITICAL(&d.mux);
        d.st.running = false;
        d.st.step = HH_DEMO_DONE;
        snprintf(d.st.note, sizeof(d.st.note), "%s", note);
        d.st.version++;
        taskEXIT_CRITICAL(&d.mux);
        d.run_req = false;
        ESP_LOGI(TAG, "[DEMO] finished: %s", note);
    }
}

esp_err_t hh_demo_start(void)
{
    if (d.task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(demo_task, "hh_demo", TASK_STACK, NULL, TASK_PRIORITY, &d.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t hh_demo_run(hh_demo_mode_t mode)
{
    if (d.task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (d.run_req) {
        return ESP_ERR_INVALID_STATE;
    }
    hh_service_status(&s_status);
    if (s_status.link != HH_LINK_ONLINE) {
        return ESP_ERR_NOT_FOUND;   /* not on the grid: nothing could be demonstrated */
    }
    d.mode_req = (uint8_t)mode;
    d.stop_req = false;
    d.run_req = true;
    xTaskNotifyGive(d.task);
    return ESP_OK;
}

void hh_demo_stop(void)
{
    d.stop_req = true;
}
