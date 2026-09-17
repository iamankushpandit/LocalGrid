#include "web_admin.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backbone.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "grid_state.h"
#include "lg_body.h"
#include "lg_crypto.h"
#include "lg_proto_config.h"
#include "node_app.h"
#include "power_trace.h"
#include "sessions.h"
#include "settings.h"

static const char *TAG = "WEB";

#define ADMIN_PBKDF2_ITERATIONS   4000u     /* measured 228 ms per 1000 on classic ESP32: ~0.9 s per login */
#define ADMIN_PASSWORD_MIN        12u
#define ADMIN_PASSWORD_MAX        64u
#define ADMIN_SESSIONS            2u
#define ADMIN_IDLE_MS             (30u * 60u * 1000u)
#define ADMIN_BODY_MAX            512u
#define TOKEN_BYTES               16u
#define TOKEN_HEX                 (TOKEN_BYTES * 2u)
#define LOGIN_FREE_FAILURES       5u
#define LOGIN_LOCK_BASE_MS        30000u
#define LOGIN_LOCK_MAX_MS         300000u

extern const char admin_html_start[] asm("_binary_admin_html_start");
extern const char admin_html_end[]   asm("_binary_admin_html_end");
extern const char icon_svg_start[]   asm("_binary_localgrid_icon_svg_start");
extern const char icon_svg_end[]     asm("_binary_localgrid_icon_svg_end");

/* ---- snapshot published by the core task ---- */

typedef struct {
    uint16_t node;
    char     node_name[16];
    uint32_t boot;
    uint32_t uptime_s;
    uint32_t grid_time;
    uint8_t  time_quality;
    uint32_t heap_free;
    uint32_t heap_min;
    uint8_t  handhelds;
    size_t   n_links;
    lgbb_link_info_t links[LG_MAX_NODES];
    size_t   n_devices;
    struct {
        uint32_t device;
        char     name[16];
        uint8_t  known;
        uint8_t  state;
        uint16_t node;
    } devices[LG_MAX_DEVICES];
    lg_roster_t roster;   /* a copy: users point at const data, groups are values (D52) */
    /* Every AP this one knows about, with the facts each announced (D45), taken on the core
     * task so the web task never reads grid_state while it changes. */
    size_t          n_aps;
    grid_ap_info_t  aps[LG_MAX_NODES];
    uint8_t         avail[LG_MAX_NODES][GRID_AVAIL_BYTES];   /* packed, two bits a minute */
    uint32_t        avail_newest_min;
    size_t          n_incidents;
    grid_incident_t incidents[GRID_INCIDENTS];
    uint32_t        time_set_unix;
    uint16_t        time_set_on;
    uint32_t        time_generation;
    uint16_t        time_from;
} snapshot_t;

typedef struct {
    bool     in_use;
    char     token[TOKEN_HEX + 1];
    char     csrf[TOKEN_HEX + 1];
    uint32_t last_ms;
} admin_session_t;

static struct {
    SemaphoreHandle_t lock;
    snapshot_t        snap;
    admin_session_t   sessions[ADMIN_SESSIONS];
    uint32_t          login_failures;
    uint32_t          locked_until_ms;
} w;

void web_admin_publish_snapshot(void)
{
    static snapshot_t s;   /* built outside the lock, copied in */
    memset(&s, 0, sizeof(s));
    uint32_t now = app_now_ms();
    s.node = g_app.index;
    strncpy(s.node_name, g_app.name, sizeof(s.node_name) - 1);
    s.boot = g_app.boot;
    s.uptime_s = now / 1000u;
    s.grid_time = app_grid_time();
    s.time_quality = g_app.time_quality;
    s.heap_free = esp_get_free_heap_size();
    s.heap_min = esp_get_minimum_free_heap_size();
    s.handhelds = sess_registered_count();
    s.n_links = lgbb_links(s.links, LG_MAX_NODES, now);
    /* Only handhelds the grid has actually seen: never list roster entries that never connected. */
    const lg_roster_t *r = g_app.core.roster;
    for (size_t i = 0; i < r->n_users && s.n_devices < LG_MAX_DEVICES; i++) {
        const lg_presence_entry_t *p = lg_node_presence(&g_app.core, r->users[i].device);
        if (p == NULL) {
            continue;
        }
        size_t k = s.n_devices++;
        s.devices[k].device = r->users[i].device;
        strncpy(s.devices[k].name, r->users[i].name, sizeof(s.devices[k].name) - 1);
        s.devices[k].known = 1;
        s.devices[k].state = p->state;
        s.devices[k].node = p->node;
    }
    s.roster = *r;
    for (uint16_t ap = 0; ap < LG_MAX_NODES; ap++) {
        if (grid_state_ap(ap, &s.aps[s.n_aps])) {
            grid_state_avail(ap, s.avail[s.n_aps]);
            s.n_aps++;
        }
    }
    s.n_incidents = grid_state_incidents(s.incidents, GRID_INCIDENTS);
    grid_state_time_info(&s.time_set_unix, &s.time_set_on, &s.time_generation);
    s.time_from = grid_state_sync_source();
    s.avail_newest_min = grid_state_avail_newest_minute();
    xSemaphoreTake(w.lock, portMAX_DELAY);
    w.snap = s;
    xSemaphoreGive(w.lock);
}

/* ---- small helpers ---- */

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static bool random_token(char *out)
{
    uint8_t raw[TOKEN_BYTES];
    if (lg_crypto_random(raw, sizeof(raw)) != 0) {
        return false;
    }
    to_hex(raw, sizeof(raw), out);
    lg_secure_zero(raw, sizeof(raw));
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    ptrace_event(PTRACE_WEB);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", message);
    return send_json(req, status, buf);
}

/* Reads the whole request body (bounded) into buf as a NUL-terminated string. */
static bool read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) {
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return true;
}

/* Minimal extractors for flat JSON objects produced by admin.html. */
static const char *json_find(const char *json, const char *key)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static bool json_string(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = json_find(json, key);
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c != '"' && c != '\\' && c != '/') {
                return false;   /* the admin UI never sends other escapes */
            }
        }
        if (n + 1 >= cap) {
            return false;
        }
        out[n++] = c;
    }
    if (*p != '"') {
        return false;
    }
    out[n] = '\0';
    return true;
}

static bool json_uint64(const char *json, const char *key, uint64_t *out)
{
    const char *p = json_find(json, key);
    if (p == NULL || !isdigit((unsigned char)*p)) {
        return false;
    }
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in != '\0' && n + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
}

/* ---- sessions, CSRF, throttling ---- */

static admin_session_t *session_from_request(httpd_req_t *req)
{
    char token[TOKEN_HEX + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, "lg_sid", token, &len) != ESP_OK || strlen(token) != TOKEN_HEX) {
        return NULL;
    }
    uint32_t now = app_now_ms();
    for (size_t i = 0; i < ADMIN_SESSIONS; i++) {
        admin_session_t *s = &w.sessions[i];
        if (!s->in_use) {
            continue;
        }
        if (now - s->last_ms > ADMIN_IDLE_MS) {
            memset(s, 0, sizeof(*s));
            continue;
        }
        if (lg_ct_equal((const uint8_t *)s->token, (const uint8_t *)token, TOKEN_HEX)) {
            s->last_ms = now;
            return s;
        }
    }
    return NULL;
}

/* Same-origin check plus CSRF header; required on every state-changing request. */
static bool csrf_ok(httpd_req_t *req, const admin_session_t *s)
{
    char origin[80], host[64], header[TOKEN_HEX + 1];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK &&
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        char expected[80];
        snprintf(expected, sizeof(expected), "http://%s", host);
        if (strcmp(origin, expected) != 0) {
            return false;
        }
    }
    if (httpd_req_get_hdr_value_str(req, "X-CSRF", header, sizeof(header)) != ESP_OK || strlen(header) != TOKEN_HEX) {
        return false;
    }
    return lg_ct_equal((const uint8_t *)s->csrf, (const uint8_t *)header, TOKEN_HEX);
}

static admin_session_t *session_create(void)
{
    admin_session_t *slot = &w.sessions[0];
    for (size_t i = 0; i < ADMIN_SESSIONS; i++) {
        if (!w.sessions[i].in_use) {
            slot = &w.sessions[i];
            break;
        }
        if (w.sessions[i].last_ms < slot->last_ms) {
            slot = &w.sessions[i];   /* evict the least recently used session */
        }
    }
    memset(slot, 0, sizeof(*slot));
    if (!random_token(slot->token) || !random_token(slot->csrf)) {
        memset(slot, 0, sizeof(*slot));
        return NULL;
    }
    slot->in_use = true;
    slot->last_ms = app_now_ms();
    return slot;
}

static esp_err_t send_session(httpd_req_t *req, const admin_session_t *s)
{
    static char cookie[96];   /* the header must outlive this call until the response is sent */
    snprintf(cookie, sizeof(cookie), "lg_sid=%s; HttpOnly; SameSite=Strict; Path=/", s->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"csrf\":\"%s\"}", s->csrf);
    return send_json(req, "200 OK", body);
}

static bool password_matches(const node_settings_t *cfg, const char *password)
{
    uint8_t hash[SETTINGS_HASH_LEN];
    if (lg_pbkdf2_sha256((const uint8_t *)password, strlen(password), cfg->salt, SETTINGS_SALT_LEN,
                         cfg->iterations, hash, sizeof(hash)) != 0) {
        return false;
    }
    bool ok = lg_ct_equal(hash, cfg->hash, sizeof(hash));
    lg_secure_zero(hash, sizeof(hash));
    return ok;
}

static bool login_locked(uint32_t *wait_s)
{
    uint32_t now = app_now_ms();
    if (w.locked_until_ms != 0 && now < w.locked_until_ms) {
        *wait_s = (w.locked_until_ms - now + 999u) / 1000u;
        return true;
    }
    return false;
}

static void login_failed(void)
{
    w.login_failures++;
    if (w.login_failures >= LOGIN_FREE_FAILURES) {
        uint32_t shift = w.login_failures - LOGIN_FREE_FAILURES;
        uint32_t lock = shift >= 4 ? LOGIN_LOCK_MAX_MS : LOGIN_LOCK_BASE_MS << shift;
        if (lock > LOGIN_LOCK_MAX_MS) {
            lock = LOGIN_LOCK_MAX_MS;
        }
        w.locked_until_ms = app_now_ms() + lock;
        ESP_LOGW(TAG, "[WEB] %" PRIu32 " failed logins; locked for %" PRIu32 " s", w.login_failures, lock / 1000u);
    }
}

static bool post_time(uint64_t unix_ms)
{
    node_cmd_t cmd = { .type = NODE_CMD_TIME_SET, .value = (uint32_t)(unix_ms / 1000u),
                       .millis = (uint16_t)(unix_ms % 1000u) };
    return xQueueSend(g_app.cmd_queue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE;
}

static bool valid_grid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > SETTINGS_GRID_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)name[i] < 0x20) {
            return false;
        }
    }
    return true;
}

static bool valid_timezone(const char *tz)
{
    size_t n = strlen(tz);
    if (n > SETTINGS_TZ_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = tz[i];
        if (!(isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '+')) {
            return false;
        }
    }
    return true;
}

/* ---- handlers ---- */

static esp_err_t h_index(httpd_req_t *req)
{
    ptrace_event(PTRACE_WEB);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, admin_html_start, admin_html_end - admin_html_start);
}

static esp_err_t h_icon(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, icon_svg_start, icon_svg_end - icon_svg_start);
}

static esp_err_t h_state(httpd_req_t *req)
{
    admin_session_t *s = session_from_request(req);
    static node_settings_t cfg;   /* httpd runs one handler at a time */
    grid_state_settings(&cfg);
    char name[2 * SETTINGS_GRID_NAME_MAX + 8];
    json_escape(cfg.grid_name, name, sizeof(name));
    char ap[2 * 16 + 8];
    json_escape(g_app.name, ap, sizeof(ap));
    char body[320];
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"logged_in\":%s,\"csrf\":\"%s\",\"grid_name\":\"%s\",\"ap\":%u,\"ap_name\":\"%s\"}",
             cfg.configured ? "true" : "false", s != NULL ? "true" : "false",
             s != NULL ? s->csrf : "", cfg.configured ? name : "", g_app.index, ap);
    return send_json(req, "200 OK", body);
}

static esp_err_t h_setup(httpd_req_t *req)
{
    static node_settings_t cfg;
    grid_state_settings(&cfg);
    if (cfg.configured) {
        return send_error(req, "409 Conflict", "LocalGrid is already set up. Log in instead.");
    }
    if (!grid_state_setup_allowed()) {
        /* Another AP may already hold the grid's settings: set up only once it has said so. */
        return send_error(req, "503 Service Unavailable", "Checking with the other APs. Try again in a few seconds.");
    }
    char body[ADMIN_BODY_MAX];
    char name[SETTINGS_GRID_NAME_MAX + 2], password[ADMIN_PASSWORD_MAX + 2], tz[SETTINGS_TZ_MAX + 2] = "";
    uint64_t unix_ms = 0;
    if (!read_body(req, body, sizeof(body)) || !json_string(body, "grid_name", name, sizeof(name)) ||
        !json_string(body, "password", password, sizeof(password)) || !json_uint64(body, "unix_ms", &unix_ms)) {
        return send_error(req, "400 Bad Request", "Grid name, password, and time are required.");
    }
    (void)json_string(body, "timezone", tz, sizeof(tz));
    lg_secure_zero(body, sizeof(body));
    if (!valid_grid_name(name)) {
        return send_error(req, "400 Bad Request", "Grid name must be 1 to 32 characters.");
    }
    if (strlen(password) < ADMIN_PASSWORD_MIN || strlen(password) > ADMIN_PASSWORD_MAX) {
        lg_secure_zero(password, sizeof(password));
        return send_error(req, "400 Bad Request", "Password must be 12 to 64 characters.");
    }
    if (unix_ms < 1700000000000ull || !valid_timezone(tz)) {
        lg_secure_zero(password, sizeof(password));
        return send_error(req, "400 Bad Request", "The browser time or time zone looks invalid.");
    }

    node_settings_t ns = { 0 };
    ns.configured = true;
    strncpy(ns.grid_name, name, SETTINGS_GRID_NAME_MAX);
    strncpy(ns.timezone, tz, SETTINGS_TZ_MAX);
    ns.iterations = ADMIN_PBKDF2_ITERATIONS;
    bool ok = lg_crypto_random(ns.salt, sizeof(ns.salt)) == 0 &&
              lg_pbkdf2_sha256((const uint8_t *)password, strlen(password), ns.salt, sizeof(ns.salt), ns.iterations,
                               ns.hash, sizeof(ns.hash)) == 0;
    lg_secure_zero(password, sizeof(password));
    if (!ok || grid_state_commit(&ns) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Could not save settings. Try again.");
    }
    (void)post_time(unix_ms);
    ESP_LOGI(TAG, "[WEB] Setup complete: grid \"%s\", time zone %s", ns.grid_name, ns.timezone);
    admin_session_t *s = session_create();
    return s != NULL ? send_session(req, s) : send_error(req, "500 Internal Server Error", "Could not start a session.");
}

static esp_err_t h_login(httpd_req_t *req)
{
    static node_settings_t cfg;
    grid_state_settings(&cfg);
    if (!cfg.configured) {
        return send_error(req, "409 Conflict", "LocalGrid is not set up yet.");
    }
    uint32_t wait_s = 0;
    if (login_locked(&wait_s)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Too many failed attempts. Try again in %" PRIu32 " seconds.", wait_s);
        return send_error(req, "429 Too Many Requests", msg);
    }
    char body[ADMIN_BODY_MAX];
    char password[ADMIN_PASSWORD_MAX + 2];
    if (!read_body(req, body, sizeof(body)) || !json_string(body, "password", password, sizeof(password))) {
        return send_error(req, "400 Bad Request", "Password is required.");
    }
    lg_secure_zero(body, sizeof(body));
    bool ok = password_matches(&cfg, password);
    lg_secure_zero(password, sizeof(password));
    if (!ok) {
        login_failed();
        return send_error(req, "401 Unauthorized", "Wrong password.");
    }
    w.login_failures = 0;
    w.locked_until_ms = 0;
    ESP_LOGI(TAG, "[WEB] Admin logged in");
    admin_session_t *s = session_create();
    return s != NULL ? send_session(req, s) : send_error(req, "500 Internal Server Error", "Could not start a session.");
}

static esp_err_t h_logout(httpd_req_t *req)
{
    admin_session_t *s = session_from_request(req);
    if (s != NULL && csrf_ok(req, s)) {
        memset(s, 0, sizeof(*s));
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "lg_sid=; Max-Age=0; HttpOnly; SameSite=Strict; Path=/");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (session_from_request(req) == NULL) {
        return send_error(req, "401 Unauthorized", "Log in to see grid status.");
    }
    static snapshot_t s;
    xSemaphoreTake(w.lock, portMAX_DELAY);
    s = w.snap;
    xSemaphoreGive(w.lock);

    static char out[4096];   /* small fields and the group table; AP history goes out as bytes (/api/history, D49) */
    static node_settings_t cfg;
    grid_state_settings(&cfg);
    char name[2 * SETTINGS_GRID_NAME_MAX + 8];
    json_escape(cfg.grid_name, name, sizeof(name));
    int n = snprintf(out, sizeof(out),
                     "{\"grid_name\":\"%s\",\"timezone\":\"%s\",\"node\":%u,\"node_name\":\"%s\",\"boot\":%" PRIu32
                     ",\"uptime_s\":%" PRIu32 ",\"grid_time\":%" PRIu32 ",\"time_quality\":%u,\"heap_free\":%" PRIu32
                     ",\"heap_min\":%" PRIu32 ",\"handhelds\":%u,\"links\":[",
                     name, cfg.timezone, s.node, s.node_name, s.boot, s.uptime_s, s.grid_time, s.time_quality,
                     s.heap_free, s.heap_min, s.handhelds);
    for (size_t i = 0; i < s.n_links && n > 0 && (size_t)n < sizeof(out); i++) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%s{\"node\":%u,\"up\":%s,\"rssi\":%d,\"age_ms\":%" PRIu32 "}",
                      i ? "," : "", s.links[i].node, s.links[i].up ? "true" : "false", s.links[i].rssi,
                      s.links[i].age_ms);
    }
    if (n > 0 && (size_t)n < sizeof(out)) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "],\"devices\":[");
    }
    for (size_t i = 0; i < s.n_devices && n > 0 && (size_t)n < sizeof(out); i++) {
        const char *state = !s.devices[i].known ? "UNKNOWN" : s.devices[i].state ? "ONLINE" : "OFFLINE";
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%s{\"device\":%" PRIu32 ",\"name\":\"%s\",\"state\":\"%s\",\"node\":%d}",
                      i ? "," : "", s.devices[i].device, s.devices[i].name, state,
                      s.devices[i].node == LG_NODE_NONE ? -1 : (int)s.devices[i].node);
    }
    /* Handhelds the grid has seen, the same list as the Handhelds card: roster places nobody has
     * taken are not offered as group members. Then the groups, members named by device (D52). */
    const lg_roster_t *r = &s.roster;
    if (n > 0 && (size_t)n < sizeof(out)) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "],\"users\":[");
    }
    for (size_t i = 0; i < s.n_devices && n > 0 && (size_t)n < sizeof(out); i++) {
        char un[2 * sizeof(s.devices[i].name) + 8];
        json_escape(s.devices[i].name, un, sizeof(un));
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%s{\"device\":%" PRIu32 ",\"name\":\"%s\"}", i ? "," : "",
                      s.devices[i].device, un);
    }
    if (n > 0 && (size_t)n < sizeof(out)) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "],\"announce_all\":%s,\"announcers\":[",
                      r->groups.announcers == LG_ANNOUNCE_EVERYONE ? "true" : "false");
    }
    for (size_t u = 0, first = 1; u < r->n_users && u < 32u && n > 0 && (size_t)n < sizeof(out); u++) {
        if (r->groups.announcers & (1u << u)) {
            n += snprintf(out + n, sizeof(out) - (size_t)n, "%s%" PRIu32, first ? "" : ",", r->users[u].device);
            first = 0;
        }
    }
    if (n > 0 && (size_t)n < sizeof(out)) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "],\"groups_version\":%" PRIu32 ",\"groups\":[", r->groups.seq);
    }
    for (size_t i = 0; i < r->groups.count && n > 0 && (size_t)n < sizeof(out); i++) {
        const lg_group_t *g = &r->groups.groups[i];
        char gn[2 * (LG_GROUP_NAME_MAX + 1u) + 8];
        json_escape(g->name, gn, sizeof(gn));
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%s{\"id\":%u,\"name\":\"%s\",\"members\":[", i ? "," : "",
                      g->id, gn);
        bool first = true;
        for (size_t u = 0; u < r->n_users && u < 32u && n > 0 && (size_t)n < sizeof(out); u++) {
            if (g->members & (1u << u)) {
                n += snprintf(out + n, sizeof(out) - (size_t)n, "%s%" PRIu32, first ? "" : ",", r->users[u].device);
                first = false;
            }
        }
        if (n > 0 && (size_t)n < sizeof(out)) {
            n += snprintf(out + n, sizeof(out) - (size_t)n, "]}");
        }
    }
    if (n > 0 && (size_t)n < sizeof(out)) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "]}");
    }
    if (n <= 0 || (size_t)n >= sizeof(out)) {
        return send_error(req, "500 Internal Server Error", "Status too large.");
    }
    return send_json(req, "200 OK", out);
}

/*
 * GET /api/history: the APs, their availability, and the outage log, as bytes (D49). The page
 * decodes it; nothing here is turned into text. Little-endian, layout 1:
 *
 * header, 16 B:  u8 layout | u8 this AP | u8 AP count | u8 incident count |
 *                u32 grid time the admin last set it (0 unknown) | u8 AP it was set on (255 unknown) |
 *                u8 AP this AP last synced from (255 never) | u16 reserved |
 *                u32 grid minute of the newest availability entry (0: not anchored to grid time)
 * AP, 76 B each: u8 AP | u8 flags (bit 0: this AP) | u8 time quality | u8 stratum |
 *                u8 last synced from (255 never) | u8 reset reason (esp_reset_reason_t) |
 *                i16 correction at that sync, ms | u32 uptime s | u32 seconds since heard |
 *                u32 seconds since that sync (0xFFFFFFFF never) | u32 boot | u32 previous run s |
 *                16 B name | 30 B availability, two bits a minute, oldest first | u16 reserved
 * incident, 16 B each, newest first: grid_incident_t exactly as stored (grid_state.h)
 */
#define HIST_HEADER  16u
#define HIST_AP      76u

static esp_err_t h_history(httpd_req_t *req)
{
    if (session_from_request(req) == NULL) {
        return send_error(req, "401 Unauthorized", "Log in to see grid status.");
    }
    static snapshot_t sn;
    xSemaphoreTake(w.lock, portMAX_DELAY);
    sn = w.snap;
    xSemaphoreGive(w.lock);

    static uint8_t out[HIST_HEADER + LG_MAX_NODES * HIST_AP + GRID_INCIDENTS * sizeof(grid_incident_t)];
    memset(out, 0, sizeof(out));
    out[0] = 1u;
    out[1] = (uint8_t)sn.node;
    out[2] = (uint8_t)sn.n_aps;
    out[3] = (uint8_t)sn.n_incidents;
    lg_wr32(out + 4, sn.time_set_unix);
    out[8] = sn.time_set_on == GRID_NO_AP ? 255u : (uint8_t)sn.time_set_on;
    out[9] = sn.time_from == GRID_NO_AP ? 255u : (uint8_t)sn.time_from;
    lg_wr32(out + 12, sn.avail_newest_min);
    size_t n = HIST_HEADER;
    for (size_t i = 0; i < sn.n_aps; i++, n += HIST_AP) {
        const grid_ap_info_t *a = &sn.aps[i];
        uint8_t *e = out + n;
        e[0] = (uint8_t)a->ap;
        e[1] = a->self ? 1u : 0u;
        e[2] = a->time_quality;
        e[3] = a->stratum;
        e[4] = a->sync_from == GRID_NO_AP ? 255u : (uint8_t)a->sync_from;
        e[5] = a->reset_reason;
        lg_wr16(e + 6, (uint16_t)a->sync_drift_ms);
        lg_wr32(e + 8, a->uptime_s);
        lg_wr32(e + 12, a->heard_age_s);
        lg_wr32(e + 16, a->sync_age_s);
        lg_wr32(e + 20, a->boot);
        lg_wr32(e + 24, a->prev_run_s);
        memcpy(e + 28, a->name, GRID_AP_NAME_MAX + 1u);
        memcpy(e + 44, sn.avail[i], GRID_AVAIL_BYTES);
    }
    memcpy(out + n, sn.incidents, sn.n_incidents * sizeof(grid_incident_t));
    n += sn.n_incidents * sizeof(grid_incident_t);
    ptrace_event(PTRACE_WEB);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)out, (ssize_t)n);
}

static esp_err_t h_time(httpd_req_t *req)
{
    admin_session_t *s = session_from_request(req);
    if (s == NULL) {
        return send_error(req, "401 Unauthorized", "Log in to set the time.");
    }
    if (!csrf_ok(req, s)) {
        return send_error(req, "403 Forbidden", "Request blocked. Reload the page and try again.");
    }
    char body[ADMIN_BODY_MAX];
    char tz[SETTINGS_TZ_MAX + 2] = "";
    uint64_t unix_ms = 0;
    if (!read_body(req, body, sizeof(body)) || !json_uint64(body, "unix_ms", &unix_ms) || unix_ms < 1700000000000ull) {
        return send_error(req, "400 Bad Request", "A valid time is required.");
    }
    static node_settings_t cfg;
    grid_state_settings(&cfg);
    if (json_string(body, "timezone", tz, sizeof(tz)) && valid_timezone(tz) && strcmp(tz, cfg.timezone) != 0) {
        memset(cfg.timezone, 0, sizeof(cfg.timezone));
        strncpy(cfg.timezone, tz, SETTINGS_TZ_MAX);
        (void)grid_state_commit(&cfg);
    }
    if (!post_time(unix_ms)) {
        return send_error(req, "503 Service Unavailable", "The AP is busy. Try again.");
    }
    ESP_LOGI(TAG, "[WEB] Admin set grid time to %" PRIu64, unix_ms / 1000u);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* Devices as the page sends them, "1,3,4", to member bits of r. False for anything else. */
static bool parse_members(const lg_roster_t *r, const char *list, uint32_t *out)
{
    uint32_t bits = 0;
    const char *p = list;
    while (*p != '\0') {
        char *end = NULL;
        unsigned long dev = strtoul(p, &end, 10);
        if (end == p) {
            return false;
        }
        int ui = lg_roster_user_index(r, (uint32_t)dev);
        if (ui < 0 || ui >= 32) {
            return false;
        }
        bits |= 1u << (unsigned)ui;
        p = end;
        if (*p == ',') {
            p++;
        } else if (*p != '\0') {
            return false;
        }
    }
    *out = bits;
    return true;
}

static esp_err_t h_groups(httpd_req_t *req)
{
    admin_session_t *s = session_from_request(req);
    if (s == NULL) {
        return send_error(req, "401 Unauthorized", "Log in to change groups.");
    }
    if (!csrf_ok(req, s)) {
        return send_error(req, "403 Forbidden", "Request blocked. Reload the page and try again.");
    }
    char body[ADMIN_BODY_MAX];
    char op[12], name[4 * (LG_GROUP_NAME_MAX + 1u)] = "", devices[160] = "";
    uint64_t id = 0;
    if (!read_body(req, body, sizeof(body)) || !json_string(body, "op", op, sizeof(op))) {
        return send_error(req, "400 Bad Request", "Say what to do with the group.");
    }
    (void)json_uint64(body, "id", &id);
    (void)json_string(body, "name", name, sizeof(name));
    (void)json_string(body, "devices", devices, sizeof(devices));

    /* Try the edit on a copy of the published table first, so the page hears why it would be
     * refused; the core task then applies it to the real one. */
    static snapshot_t snap;
    xSemaphoreTake(w.lock, portMAX_DELAY);
    snap = w.snap;
    xSemaphoreGive(w.lock);
    node_cmd_t cmd = { .type = NODE_CMD_GROUP_EDIT };
    lg_group_edit_t *e = &cmd.group;
    if (strcmp(op, "create") == 0) {
        e->op = LG_GROUP_CREATE;
    } else if (strcmp(op, "update") == 0) {
        e->op = LG_GROUP_UPDATE;
    } else if (strcmp(op, "delete") == 0) {
        e->op = LG_GROUP_DELETE;
    } else if (strcmp(op, "announcers") == 0) {
        /* Who may announce (D56). "all" keeps every handheld, including ones not seen yet. */
        e->op = LG_GROUP_ANNOUNCERS;
        if (strcmp(devices, "all") == 0) {
            e->members = LG_ANNOUNCE_EVERYONE;
        } else if (!parse_members(&snap.roster, devices, &e->members)) {
            return send_error(req, "400 Bad Request", "Pick handhelds from the list.");
        }
    } else {
        return send_error(req, "400 Bad Request", "Unknown group action.");
    }
    e->id = id <= 0xFFFFu ? (uint16_t)id : 0u;
    if (e->op != LG_GROUP_DELETE && e->op != LG_GROUP_ANNOUNCERS) {
        if (strlen(name) == 0 || strlen(name) > LG_GROUP_NAME_MAX) {
            return send_error(req, "400 Bad Request", "A group name is 1 to 15 bytes (fewer with accents or emoji).");
        }
        memcpy(e->name, name, strlen(name));
        if (!parse_members(&snap.roster, devices, &e->members)) {
            return send_error(req, "400 Bad Request", "Pick handhelds from the list.");
        }
    }
    switch (lg_groups_apply_edit(&snap.roster, 0, e, g_app.index)) {
    case 0:
        break;
    case LG_ACK_REJ_UNKNOWN_TARGET:
        return send_error(req, "404 Not Found", "That group no longer exists. Reload the page.");
    default:
        if (e->op == LG_GROUP_CREATE && snap.roster.groups.count >= LG_MAX_GROUPS) {
            return send_error(req, "409 Conflict", "There are already 8 groups. Remove one first.");
        }
        return send_error(req, "400 Bad Request", "Check the name, and pick at least one handheld.");
    }
    if (xQueueSend(g_app.cmd_queue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        return send_error(req, "503 Service Unavailable", "The AP is busy. Try again.");
    }
    if (e->op == LG_GROUP_ANNOUNCERS) {
        ESP_LOGI(TAG, "[WEB] Admin announcers: 0x%08" PRIX32, e->members);
    } else {
        ESP_LOGI(TAG, "[WEB] Admin group %s: id %u \"%s\"", op, e->id, e->name);
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t web_admin_start(void)
{
    w.lock = xSemaphoreCreateMutex();
    if (w.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = h_index },
        { .uri = "/favicon.svg",  .method = HTTP_GET,  .handler = h_icon },
        { .uri = "/api/state",    .method = HTTP_GET,  .handler = h_state },
        { .uri = "/api/setup",    .method = HTTP_POST, .handler = h_setup },
        { .uri = "/api/login",    .method = HTTP_POST, .handler = h_login },
        { .uri = "/api/logout",   .method = HTTP_POST, .handler = h_logout },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/history",  .method = HTTP_GET,  .handler = h_history },
        { .uri = "/api/time",     .method = HTTP_POST, .handler = h_time },
        { .uri = "/api/groups",   .method = HTTP_POST, .handler = h_groups },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    static node_settings_t cfg;
    grid_state_settings(&cfg);
    uint8_t ip[4];
    lg_proto_node_ip(g_app.index, ip);
    ESP_LOGI(TAG, "[WEB] Admin page at http://%u.%u.%u.%u/ (%s)", ip[0], ip[1], ip[2], ip[3],
             cfg.configured ? "configured" : "setup mode");
    return ESP_OK;
}
