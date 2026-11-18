/*
 * homey_daemon.c - FreeToon integration: start Homey flows, show devices.
 *
 * Talks to the Homey's local Web API:  http://<host>/api/manager/...
 * with header  Authorization: Bearer <token>  (a Homey Pro API key, or a
 * session token; see README.md).
 *
 *   every 5 min   GET flow/flow/ and flow/advancedflow/  -> flows_all.json
 *                 (every flow Homey can start by hand, for the picker)
 *   every `poll`  GET devices/device/                      -> devices_all.json
 *   commands from the UI plugin (cmd-*.json):
 *     {"cmd":"trigger","id":"<flow id>","adv":0|1}
 *        POST flow/flow/<id>/trigger  or  flow/advancedflow/<id>/trigger
 *     {"cmd":"set","id":"<device id>","cap":"onoff","value":true|false}
 *        PUT devices/device/<id>/capability/onoff  {"value":...}
 *     {"cmd":"refresh"}
 *
 * Output: /var/volatile/tmp/homey/state.json with the chosen flows and
 * devices (config keys `flows` and `devices`, comma-separated ids, in that
 * order) and the result of the last command; the tile shows how many of the
 * chosen devices are on and the last flow started.
 *
 * Settings: /mnt/data/integrations/homey/homey.conf, written by the UI.
 */
#define _GNU_SOURCE
#define FTAPP_ID "homey"
#include "ftapp.h"
#include "ftjson.h"

#include <ctype.h>

#define CONF_PATH    FTAPP_DIR "/homey.conf"
#define STATE_PATH   FTAPP_RUN "/state.json"
#define FLOWS_PATH   FTAPP_RUN "/flows_all.json"
#define DEVICES_PATH FTAPP_RUN "/devices_all.json"
#define MAX_SEL      64
#define TOKEN_N      1100            /* session tokens can be long JWTs */

typedef struct {
    char host[96];
    char token[TOKEN_N];
    char flows[MAX_SEL * 40];
    char devices[MAX_SEL * 40];
    int  poll_s;
    /* older Homeys (no API keys): the app renews its own session token */
    char cloud_refresh[512];  /* Athom OAuth refresh token (tools/homey-login.sh) */
    char homey_id[32];        /* Homey cloud id */
    long token_at;            /* when `token` was made (unix s) */
    int  autorun;            /* 1 = run the rules below (master switch, off by default) */
    char on_state[4][48];    /* flow to start when the Toon switches to comfort/home/sleep/away */
    char at_time[2][8];      /* "07:00" */
    char at_flow[2][48];
} conf_t;

static const char * STATE_KEYS[4] = { "on_comfort", "on_home", "on_sleep", "on_away" };

static conf_t g_conf;
static long   g_conf_mtime = -1;
static char   g_err[120];
static jv *   g_flows;              /* merged list [{id,name,adv,enabled}] */
static char * g_flows_json;
static int    g_token_refused;      /* last Homey call answered 401 */
static char * g_sel_devs;           /* JSON array of the chosen devices (compact) */
static int    g_on_count, g_with_onoff;
static time_t g_dev_at, g_flow_at;       /* last success */
static time_t g_dev_try, g_flow_try;     /* last attempt (also on failure) */
static char   g_last_id[48], g_last_name[64];
static int    g_last_ok = -1;
static time_t g_last_at;

static int load_conf(void) {
    long mt = ft_mtime(CONF_PATH);
    if (mt == g_conf_mtime) return 0;
    g_conf_mtime = mt;
    conf_t c;
    memset(&c, 0, sizeof c);
    c.poll_s = 20;
    char * t = ft_read_file(CONF_PATH, NULL);
    if (t) {
        char v[16];
        ft_conf_get(t, "host", c.host, sizeof c.host);
        ft_conf_get(t, "token", c.token, sizeof c.token);
        ft_conf_get(t, "flows", c.flows, sizeof c.flows);
        ft_conf_get(t, "devices", c.devices, sizeof c.devices);
        if (ft_conf_get(t, "poll", v, sizeof v)) c.poll_s = atoi(v);
        if (ft_conf_get(t, "auto", v, sizeof v)) c.autorun = atoi(v) == 1;
        ft_conf_get(t, "cloud_refresh", c.cloud_refresh, sizeof c.cloud_refresh);
        ft_conf_get(t, "homey_id", c.homey_id, sizeof c.homey_id);
        char ta[24];
        if (ft_conf_get(t, "token_at", ta, sizeof ta)) c.token_at = atol(ta);
        for (int i = 0; i < 4; i++) ft_conf_get(t, STATE_KEYS[i], c.on_state[i], sizeof c.on_state[i]);
        for (int i = 0; i < 2; i++) {
            char k[16];
            snprintf(k, sizeof k, "at%d_time", i + 1);
            ft_conf_get(t, k, c.at_time[i], sizeof c.at_time[i]);
            snprintf(k, sizeof k, "at%d_flow", i + 1);
            ft_conf_get(t, k, c.at_flow[i], sizeof c.at_flow[i]);
            int h, m;
            if (sscanf(c.at_time[i], "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60)
                snprintf(c.at_time[i], sizeof c.at_time[i], "%02d:%02d", h, m);
            else c.at_time[i][0] = 0;
        }
        free(t);
    } else {
        const char * def =
            "# Homey for FreeToon. Edit on the Toon (tile > settings) or here.\n"
            "# host: the Homey's IP address on your network, e.g. 192.168.1.30\n"
            "host=\n"
            "# token: Homey Pro API key (Settings > API keys), or a session token\n"
            "token=\n"
            "# flows / devices: comma-separated ids, chosen in the app\n"
            "flows=\n"
            "devices=\n"
            "poll=20\n"
            "# automatic starts (all off by default). auto=1 switches them on.\n"
            "# on_comfort/on_home/on_sleep/on_away: flow id to start when the Toon\n"
            "# switches to that program state (button or schedule); at1_time=07:00 + at1_flow=<id>\n"
            "auto=0\n"
            "on_comfort=\non_home=\non_sleep=\non_away=\n"
            "at1_time=\nat1_flow=\nat2_time=\nat2_flow=\n"
            "# older Homey without API keys: tools/homey-login.sh fills these and the\n"
            "# app then renews `token` itself (every 20 h, and when Homey refuses it)\n"
            "cloud_refresh=\nhomey_id=\ntoken_at=0\n";
        ft_write_atomic(CONF_PATH, def, strlen(def), 1);
        g_conf_mtime = ft_mtime(CONF_PATH);
    }
    if (c.poll_s < 10) c.poll_s = 10;
    /* allow "http://1.2.3.4/" as well as "1.2.3.4" */
    char * h = c.host;
    if (!strncmp(h, "http://", 7)) memmove(h, h + 7, strlen(h + 7) + 1);
    size_t hl = strlen(h);
    while (hl > 0 && h[hl - 1] == '/') h[--hl] = 0;
    for (char * p = h; *p; p++) if (isspace((unsigned char)*p) || *p == '"' || *p == '\\' || *p == '?') { h[0] = 0; break; }
    g_conf = c;
    ftlog("config: host=%s %s flows=%d chars devices=%d chars", c.host[0] ? c.host : "(none)",
          c.token[0] ? "(token set)" : "(no token)", (int)strlen(c.flows), (int)strlen(c.devices));
    return 1;
}

/* ---- config writes (token renewal) -------------------------------------------------- */

static void conf_write_key(const char * key, const char * value) {
    /* values come from network answers: a line break would add config lines */
    for (const unsigned char * p = (const unsigned char *)value; *p; p++)
        if (*p < 0x20 || *p == 0x7f) { ftlog("not saving %s: control character in the value", key); return; }
    char * t = ft_read_file(CONF_PATH, NULL);
    size_t cap = (t ? strlen(t) : 0) + strlen(key) + strlen(value) + 8, o = 0, kl = strlen(key);
    char * out = (char *)malloc(cap);
    int done = 0;
    if (!out) { free(t); return; }
    for (char * line = t; line && *line; ) {
        char * eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        if (ll > kl && !strncmp(line, key, kl) && line[kl] == '=') {
            if (!done) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
            done = 1;
        } else { memcpy(out + o, line, ll); o += ll; out[o++] = '\n'; }
        line = eol ? eol + 1 : NULL;
    }
    if (!done) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
    ft_write_atomic(CONF_PATH, out, o, 1);       /* 0600: it holds the tokens */
    free(out);
    free(t);
}

/* ---- HTTP --------------------------------------------------------------------- */

static int id_ok(const char * s) {
    if (!s || !*s || strlen(s) > 40) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-') return 0;
    return 1;
}

static int homey_get(const char * path, char ** body) {
    char url[256];
    static char auth[TOKEN_N + 32];
    snprintf(url, sizeof url, "http://%s%s", g_conf.host, path);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_conf.token);
    const char * headers[] = { auth, "Accept: application/json", NULL };
    return ft_http_get(url, headers, 10, body, 4 * 1024 * 1024);
}

static int homey_send(const char * method, const char * path, const char * json, char ** body) {
    char url[256];
    static char auth[TOKEN_N + 32];
    snprintf(url, sizeof url, "http://%s%s", g_conf.host, path);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_conf.token);
    const char * headers[] = { auth, NULL };
    return ft_http_send(method, url, headers, json, 10, body);
}

static void set_err_for(int code) {
    int en = ft_english();
    if (code == 401) g_token_refused = 1;
    if (code == 0) snprintf(g_err, sizeof g_err, "%s", en ? "Homey unreachable" : "Homey niet bereikbaar");
    else if (code == 401) snprintf(g_err, sizeof g_err, "%s", en ? "Homey token refused or expired" : "Homey-token geweigerd of verlopen");
    else if (code == 403) snprintf(g_err, sizeof g_err, "%s", en ? "Homey token lacks rights" : "Homey-token mist rechten");
    else snprintf(g_err, sizeof g_err, "Homey HTTP %d", code);
}

/* ---- bounded JSON building --------------------------------------------------------- */

typedef struct { char * b; size_t cap, len; int full; } jbuf_t;
static void jb_add(jbuf_t * j, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_add(jbuf_t * j, const char * fmt, ...) {
    if (j->full) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(j->b + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (r < 0 || j->len + (size_t)r >= j->cap) { j->full = 1; j->b[j->len] = 0; return; }
    j->len += (size_t)r;
}
static void jb_str(jbuf_t * j, const char * v) {
    if (j->full) return;
    int r = jesc(j->b, j->cap, j->len, v);
    if (r < 0) { j->full = 1; j->b[j->len] = 0; return; }
    j->len = (size_t)r;
}

/* ---- session renewal (Athom cloud -> delegation token -> Homey login) ---------------- */

/* The official Homey CLI's OAuth client (public in its npm package); the
 * refresh token from tools/homey-login.sh belongs to it. */
#define ATHOM_API        "https://api.athom.com"
#define ATHOM_CLIENT_ID  "64691b4358336640a5ecee5c"
#define ATHOM_CLIENT_SEC "ed09f559ae12b1522d00431f0bf7c5755603c41e"
#define RENEW_AFTER_S    (20 * 3600)            /* sessions live 24 h */

static time_t g_renew_try;

static void b64(const char * in, char * out, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t il = strlen(in), o = 0;
    for (size_t i = 0; i < il && o + 5 < n; i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (i + 1 < il ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < il ? (unsigned char)in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < il ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < il ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static void form_enc(const char * in, char * out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char * p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

/* Tokens are base64url / hex / JWT-like: letters, digits, - _ . : only. */
static int token_chars_ok(const char * s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.' && *s != ':') return 0;
    return 1;
}

/* 1 when a new session token was stored. */
static int renew_session(void) {
    if (!g_conf.cloud_refresh[0] || !g_conf.homey_id[0] || !g_conf.host[0] || !id_ok(g_conf.homey_id)) return 0;
    time_t now = time(NULL);
    if (now - g_renew_try < 600) return 0;          /* at most every 10 min when it fails */
    g_renew_try = now;

    /* 1. refresh token -> access token (a new refresh token may come back) */
    char basic[160], auth[200], form[1200], enc[1100];
    b64(ATHOM_CLIENT_ID ":" ATHOM_CLIENT_SEC, basic, sizeof basic);
    snprintf(auth, sizeof auth, "Authorization: Basic %s", basic);
    form_enc(g_conf.cloud_refresh, enc, sizeof enc);
    snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s", enc);
    const char * h1[] = { auth, "Accept: application/json", NULL };
    char * body = NULL;
    int code = ft_http_body("POST", ATHOM_API "/oauth2/token", h1, "application/x-www-form-urlencoded", form, 20, &body);
    jv * r = code == 200 ? jparse(body) : NULL;
    free(body);
    const char * access = jstr(r, "access_token", "");
    const char * refresh = jstr(r, "refresh_token", "");
    if (!access[0]) {
        ftlog("token renewal: Athom refused the refresh token (HTTP %d); run tools/homey-login.sh again", code);
        snprintf(g_err, sizeof g_err, "%s", ft_english() ? "Homey login expired: log in again" : "Homey-login verlopen: opnieuw inloggen");
        jfree(r);
        return 0;
    }
    if (refresh[0] && !token_chars_ok(refresh)) refresh = "";      /* never store anything odd */
    if (refresh[0] && strcmp(refresh, g_conf.cloud_refresh) != 0) {
        snprintf(g_conf.cloud_refresh, sizeof g_conf.cloud_refresh, "%s", refresh);
        conf_write_key("cloud_refresh", refresh);  /* store it before anything else can fail */
    }

    /* 2. access token -> delegation token for this Homey */
    static char bearer[1200];
    snprintf(bearer, sizeof bearer, "Authorization: Bearer %s", access);
    jfree(r);
    char url[200];
    snprintf(url, sizeof url, ATHOM_API "/delegation/token?audience=homey&homeyId=%s", g_conf.homey_id);
    const char * h2[] = { bearer, "Accept: application/json", NULL };
    code = ft_http_body("POST", url, h2, "application/json", "{}", 20, &body);
    jv * jwt = code >= 200 && code < 300 ? jparse(body) : NULL;
    free(body);
    if (!jwt || jwt->type != JV_STR) { ftlog("token renewal: no delegation token (HTTP %d)", code); jfree(jwt); return 0; }

    /* 3. delegation token -> session on the Homey itself (local API) */
    size_t ll = strlen(jwt->s) + 32;
    char * login = (char *)malloc(ll);
    if (!login) { jfree(jwt); return 0; }
    for (const char * p = jwt->s; *p; p++)              /* a JWT is base64url + dots */
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') { free(login); jfree(jwt); return 0; }
    snprintf(login, ll, "{\"token\":\"%s\"}", jwt->s);
    jfree(jwt);
    snprintf(url, sizeof url, "http://%s/api/manager/users/login", g_conf.host);
    code = ft_http_body("POST", url, NULL, "application/json", login, 15, &body);
    free(login);
    jv * tok = code == 200 ? jparse(body) : NULL;
    free(body);
    if (!tok || tok->type != JV_STR || strlen(tok->s) >= sizeof g_conf.token || !token_chars_ok(tok->s)) {
        ftlog("token renewal: Homey login failed (HTTP %d)", code);
        jfree(tok);
        return 0;
    }
    snprintf(g_conf.token, sizeof g_conf.token, "%s", tok->s);
    jfree(tok);
    g_conf.token_at = now;
    conf_write_key("token", g_conf.token);
    char ts[24];
    snprintf(ts, sizeof ts, "%ld", (long)now);
    conf_write_key("token_at", ts);
    g_token_refused = 0;
    g_err[0] = 0;
    ftlog("token renewal: new Homey session");
    return 1;
}

/* ---- flows -------------------------------------------------------------------- */

static void fetch_flows(void) {
    jbuf_t j = { (char *)malloc(64 * 1024), 64 * 1024, 0, 0 };
    if (!j.b) return;
    jb_add(&j, "[");
    int n = 0, ok = 0;
    static const char * kinds[2] = { "/api/manager/flow/flow/", "/api/manager/flow/advancedflow/" };
    for (int adv = 0; adv < 2; adv++) {
        char * body = NULL;
        int code = homey_get(kinds[adv], &body);
        if (code != 200) { if (!adv) set_err_for(code); free(body); continue; }
        ok = 1;
        jv * r = jparse(body);
        for (jv * f = jfirst(r); f && n < 4 * MAX_SEL; f = f->next) {
            if (!jbool(f, "triggerable", 0)) continue;
            size_t mark = j.len;
            jb_add(&j, "%s{\"id\":", n ? "," : "");
            jb_str(&j, jstr(f, "id", ""));
            jb_add(&j, ",\"name\":");
            jb_str(&j, jstr(f, "name", "?"));
            jb_add(&j, ",\"adv\":%d,\"enabled\":%d}", adv, jbool(f, "enabled", 1));
            if (j.full) { j.len = mark; j.b[mark] = 0; j.full = 0; break; }   /* drop the half entry */
            n++;
        }
        jfree(r);
        free(body);
    }
    if (j.cap - j.len < 4) { j.len = j.cap - 4; }
    j.b[j.len++] = ']'; j.b[j.len++] = '\n'; j.b[j.len] = 0;
    if (!ok) { free(j.b); return; }
    g_err[0] = 0;
    ft_write_atomic(FLOWS_PATH, j.b, j.len, 0);
    jfree(g_flows);
    g_flows = jparse(j.b);
    free(g_flows_json);
    g_flows_json = j.b;
    g_flow_at = time(NULL);
    ftlog("%d flows can be started", n);
}

static const jv * flow_by_id(const char * id) {
    for (jv * f = jfirst(g_flows); f; f = f->next) if (!strcmp(jstr(f, "id", ""), id)) return f;
    return NULL;
}

/* ---- devices -------------------------------------------------------------------- */

static int in_list(const char * list, const char * id) {
    size_t n = strlen(id);
    if (!n) return 0;
    for (const char * p = list; (p = strstr(p, id)); p += n)
        if ((p == list || p[-1] == ',' || p[-1] == ' ') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

/* Reads all devices once, keeps only a compact picker list (file) and the
 * chosen devices (g_sel_devs, in config order); the full tree is freed. */
static void fetch_devices(void) {
    char * body = NULL;
    int code = homey_get("/api/manager/devices/device/", &body);
    if (code != 200) { set_err_for(code); free(body); return; }
    jv * r = jparse(body);
    free(body);
    if (!r) { snprintf(g_err, sizeof g_err, "%s", ft_english() ? "Homey answer too large or invalid" : "Homey-antwoord te groot of ongeldig"); return; }
    g_dev_at = time(NULL);
    g_err[0] = 0;

    jbuf_t pk = { (char *)malloc(64 * 1024), 64 * 1024, 0, 0 };
    if (pk.b) {
        jb_add(&pk, "[");
        int n = 0;
        for (jv * d = jfirst(r); d; d = d->next) {
            size_t mark = pk.len;
            jb_add(&pk, "%s{\"id\":", n ? "," : "");
            jb_str(&pk, jstr(d, "id", ""));
            jb_add(&pk, ",\"name\":");
            jb_str(&pk, jstr(d, "name", "?"));
            jb_add(&pk, ",\"zone\":");
            jb_str(&pk, jstr(d, "zoneName", ""));
            jb_add(&pk, ",\"onoff\":%d}", jpath(d, "capabilitiesObj.onoff") ? 1 : 0);
            if (pk.full) { pk.len = mark; pk.b[mark] = 0; break; }
            n++;
        }
        if (pk.cap - pk.len < 4) pk.len = pk.cap - 4;
        pk.b[pk.len++] = ']'; pk.b[pk.len++] = '\n'; pk.b[pk.len] = 0;
        ft_write_atomic(DEVICES_PATH, pk.b, pk.len, 0);
        free(pk.b);
    }

    jbuf_t sel = { (char *)malloc(32 * 1024), 32 * 1024, 0, 0 };
    if (!sel.b) { jfree(r); return; }
    jb_add(&sel, "[");
    char ids[sizeof g_conf.devices];
    snprintf(ids, sizeof ids, "%s", g_conf.devices);
    int n = 0;
    g_on_count = g_with_onoff = 0;
    for (char * id = strtok(ids, ","); id; id = strtok(NULL, ",")) {
        while (*id == ' ') id++;
        const jv * d = NULL;
        for (jv * x = jfirst(r); x; x = x->next) if (!strcmp(jstr(x, "id", ""), id)) { d = x; break; }
        if (!d) continue;
        jv * caps = jget(d, "capabilitiesObj");
        jv * onoff = jpath(caps, "onoff.value");
        int on = onoff && onoff->type == JV_BOOL ? (int)onoff->n : -1;
        if (on >= 0) { g_with_onoff++; g_on_count += on; }
        size_t mark = sel.len;
        jb_add(&sel, "%s{\"id\":", n ? "," : "");
        jb_str(&sel, id);
        jb_add(&sel, ",\"name\":");
        jb_str(&sel, jstr(d, "name", "?"));
        jb_add(&sel, ",\"zone\":");
        jb_str(&sel, jstr(d, "zoneName", ""));
        jb_add(&sel, ",\"class\":");
        jb_str(&sel, jstr(d, "class", ""));
        jb_add(&sel, ",\"on\":%d,\"power\":%.0f,\"temp\":%.1f,\"available\":%d}", on,
               jnum(caps, "measure_power.value", -1), jnum(caps, "measure_temperature.value", -999), jbool(d, "available", 1));
        if (sel.full) { sel.len = mark; sel.b[mark] = 0; break; }
        n++;
    }
    if (sel.cap - sel.len < 2) sel.len = sel.cap - 2;
    sel.b[sel.len++] = ']'; sel.b[sel.len] = 0;
    free(g_sel_devs);
    g_sel_devs = sel.b;
    jfree(r);
}

/* ---- commands -------------------------------------------------------------------- */

static void run_cmd(const char * text) {
    jv * c = jparse(text);
    const char * cmd = jstr(c, "cmd", "");
    const char * id = jstr(c, "id", "");
    char path[160];
    char * body = NULL;
    if (!strcmp(cmd, "trigger") && id_ok(id)) {
        int adv = jbool(c, "adv", 0);
        snprintf(path, sizeof path, "/api/manager/flow/%s/%s/trigger", adv ? "advancedflow" : "flow", id);
        int code = homey_send("POST", path, "{}", &body);
        const jv * f = flow_by_id(id);
        snprintf(g_last_id, sizeof g_last_id, "%s", id);
        snprintf(g_last_name, sizeof g_last_name, "%s", f ? jstr(f, "name", id) : id);
        g_last_ok = code >= 200 && code < 300;
        g_last_at = time(NULL);
        ftlog("start flow '%s': HTTP %d", g_last_name, code);
        if (!g_last_ok) set_err_for(code);
    } else if (!strcmp(cmd, "set") && id_ok(id) && !strcmp(jstr(c, "cap", ""), "onoff")) {
        snprintf(path, sizeof path, "/api/manager/devices/device/%s/capability/onoff", id);
        int on = jbool(c, "value", 0);
        int code = homey_send("PUT", path, on ? "{\"value\":true}" : "{\"value\":false}", &body);
        ftlog("device %s onoff=%d: HTTP %d", id, on, code);
        if (code < 200 || code >= 300) set_err_for(code);
        g_dev_try = 0;                                  /* re-read states now */
    } else if (!strcmp(cmd, "refresh")) {
        g_dev_try = 0;
        g_flow_try = 0;
    }
    free(body);
    jfree(c);
}

/* ---- automatic starts ------------------------------------------------------------- */

static void start_flow(const char * id, const char * why) {
    if (!id_ok(id)) return;
    const jv * f = flow_by_id(id);
    if (!f) { ftlog("rule %s: flow %s is not (or no longer) startable", why, id); return; }
    char cmd[160];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"trigger\",\"id\":\"%s\",\"adv\":%d}", id, (int)jnum(f, "adv", 0));
    ftlog("rule %s: starting '%s'", why, jstr(f, "name", id));
    run_cmd(cmd);
}

/* Toon program state from the thermostat app: 0 comfort, 1 home, 2 sleep, 3 away, -1 manual. */
static int toon_state(void) {
    char * body = NULL;
    const char * headers[] = { NULL };
    int code = ft_http_get("http://127.0.0.1/happ_thermstat?action=getThermostatInfo", headers, 5, &body, 64 * 1024);
    int st = -2;
    if (code == 200 && body) {
        jv * r = jparse(body);
        if (r) st = (int)jnum(r, "activeState", -2);
        jfree(r);
    }
    free(body);
    return st;
}

static void rules_tick(void) {
    static int last_state = -2;
    static time_t last_poll;
    static int fired_day[2] = { -1, -1 };
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    int any = 0;
    for (int i = 0; i < 4; i++) any |= g_conf.on_state[i][0] != 0;
    if (!g_conf.autorun || !any) last_state = -2;      /* re-learn the state when switched on */
    else if (now - last_poll >= 10) {
        last_poll = now;
        int st = toon_state();
        if (st >= -1) {
            if (last_state >= -1 && st != last_state && st >= 0 && st <= 3 && g_conf.autorun && g_conf.on_state[st][0]) {
                static const char * names[4] = { "comfort", "home", "sleep", "away" };
                start_flow(g_conf.on_state[st], names[st]);
            }
            last_state = st;
        }
    }
    char hm[8];
    snprintf(hm, sizeof hm, "%02d:%02d", lt.tm_hour, lt.tm_min);
    for (int i = 0; i < 2; i++) {
        if (!g_conf.at_time[i][0] || strcmp(hm, g_conf.at_time[i]) != 0 || fired_day[i] == lt.tm_yday) continue;
        fired_day[i] = lt.tm_yday;
        if (g_conf.autorun && g_conf.at_flow[i][0]) start_flow(g_conf.at_flow[i], g_conf.at_time[i]);
    }
}

/* ---- publish ---------------------------------------------------------------------- */

static void publish(void) {
    int en = ft_english();
    time_t now = time(NULL);
    size_t cap = 48 * 1024, len = 0;
    char * b = (char *)malloc(cap);
    if (!b) return;
#define APP(...) do { int r_ = snprintf(b + len, cap - len, __VA_ARGS__); if (r_ > 0 && len + (size_t)r_ < cap) len += (size_t)r_; } while (0)
#define STR(v) do { int r_ = jesc(b, cap, len, v); if (r_ > 0) len = (size_t)r_; } while (0)
    APP("{\"configured\":%d,\"error\":", g_conf.host[0] && g_conf.token[0]);
    STR(g_err);
    APP(",\"flows\":[");
    char sel[sizeof g_conf.flows];
    snprintf(sel, sizeof sel, "%s", g_conf.flows);
    int n = 0;
    for (char * id = strtok(sel, ","); id; id = strtok(NULL, ",")) {
        while (*id == ' ') id++;
        const jv * f = flow_by_id(id);
        if (!f) continue;
        APP("%s{\"id\":", n++ ? "," : ""); STR(id);
        APP(",\"name\":"); STR(jstr(f, "name", "?"));
        APP(",\"adv\":%d,\"enabled\":%d}", (int)jnum(f, "adv", 0), jbool(f, "enabled", 1));
    }
    int n_flows = n;
    APP("],\"devices\":%s", g_sel_devs ? g_sel_devs : "[]");
    APP(",\"last\":{\"id\":"); STR(g_last_id);
    APP(",\"name\":"); STR(g_last_name);
    APP(",\"ok\":%d,\"at\":%ld}}\n", g_last_ok, (long)g_last_at);
#undef APP
#undef STR
    ft_write_atomic(STATE_PATH, b, len, 0);
    free(b);

    char value[32], subtitle[128];
    if (!g_conf.host[0] || !g_conf.token[0]) {
        snprintf(value, sizeof value, "Homey");
        snprintf(subtitle, sizeof subtitle, "%s", en ? "set up in the app" : "instellen in de app");
    } else if (g_err[0]) {
        snprintf(value, sizeof value, "Homey");
        snprintf(subtitle, sizeof subtitle, "%s", g_err);
    } else {
        if (g_with_onoff) snprintf(value, sizeof value, en ? "%d on" : "%d aan", g_on_count);
        else snprintf(value, sizeof value, "%d flows", n_flows);
        if (g_last_ok == 1 && now - g_last_at < 3600) {
            struct tm tm;
            localtime_r(&g_last_at, &tm);
            snprintf(subtitle, sizeof subtitle, en ? "%s started %02d:%02d" : "%s gestart %02d:%02d", g_last_name, tm.tm_hour, tm.tm_min);
        } else {
            snprintf(subtitle, sizeof subtitle, "%s", en ? "tap to start a flow" : "tik om een flow te starten");
        }
    }
    ftapp_tile(value, subtitle, "");
}

/* ---- main -------------------------------------------------------------------------- */

int main(int argc, char ** argv) {
    int once = argc > 1 && !strcmp(argv[1], "--once");
    ftapp_init();
    load_conf();
    if (once) {
        if (g_conf.host[0] && g_conf.token[0]) { fetch_flows(); fetch_devices(); }
        publish();
        return g_err[0] ? 1 : 0;
    }
    time_t last_pub = 0;
    while (ftapp_running) {
        if (load_conf()) { g_flow_try = 0; g_dev_try = 0; }
        ftapp_bxt_service();
        if (g_conf.cloud_refresh[0] && (g_token_refused || !g_conf.token[0] || time(NULL) - g_conf.token_at > RENEW_AFTER_S)) {
            if (renew_session()) { g_flow_try = 0; g_dev_try = 0; }
        }
        char * cmd;
        int ran = 0;
        while ((cmd = ftapp_next_cmd())) { if (g_conf.host[0] && g_conf.token[0]) run_cmd(cmd); free(cmd); ran = 1; }
        time_t now = time(NULL);
        int fresh = 0;
        if (g_conf.host[0] && g_conf.token[0]) {
            if (now - g_flow_try >= 300) { g_flow_try = now; fetch_flows(); fresh = 1; }
            if (now - g_dev_try >= g_conf.poll_s) { g_dev_try = now; fetch_devices(); fresh = 1; }
        }
        if (g_conf.host[0] && g_conf.token[0]) {
            const char * before = g_last_id[0] ? g_last_id : "";
            char prev[48];
            snprintf(prev, sizeof prev, "%s%ld", before, (long)g_last_at);
            rules_tick();
            char after[48];
            snprintf(after, sizeof after, "%s%ld", g_last_id, (long)g_last_at);
            if (strcmp(prev, after) != 0) ran = 1;
        }
        if (ran || fresh || now - last_pub >= 15) { publish(); last_pub = now; }
        usleep(300 * 1000);
    }
    ftlog("stopping");
    return 0;
}
