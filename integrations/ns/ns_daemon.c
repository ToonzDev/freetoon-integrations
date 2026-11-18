/*
 * ns_daemon.c - FreeToon integration: NS departures (Dutch Railways).
 *
 * Every `poll` seconds (default 60) asks the NS Reisinformatie API for the
 * departures from one station:
 *   GET https://gateway.apiportal.ns.nl/reisinformatie-api/api/v2/departures?station=<code>
 *   header Ocp-Apim-Subscription-Key: <api_key>   (apiportal.ns.nl, product "Ns-App")
 * keeps the trains that match the direction filter and that you can still
 * catch (walk time), and publishes
 *   - the tile: <value> the next departure ("10:16") and <subtitle>
 *     ("+2  IC Amsterdam C.  spoor 7");
 *   - /var/volatile/tmp/ns/state.json for the UI plugin (ns_ui.<toon>.so).
 *
 * The station can be typed as a name ("Utrecht", "Amersfoort Centraal") or a
 * code ("UT"); a name is looked up once with the NS stations API and the code
 * cached in the config.
 *
 * Without an API key (or with mock=1) it shows sample departures in the same
 * format, generated around the current time, so the app can be tried before
 * a key exists. The UI marks them as an example.
 *
 * Settings: /mnt/data/integrations/ns/ns.conf (key=value), written by the UI.
 */
#define _GNU_SOURCE
#define FTAPP_ID "ns"
#include "ftapp.h"
#include "ftjson.h"

#include <ctype.h>

#define CONF_PATH  FTAPP_DIR "/ns.conf"
#define STATE_PATH FTAPP_RUN "/state.json"
#define API_BASE   "https://gateway.apiportal.ns.nl"
#define MAX_TRAINS 12

typedef struct {
    char api_key[80];
    char station[64];        /* what the user typed */
    char station_code[16];   /* resolved */
    char station_name[64];
    char direction[128];     /* comma-separated alternatives, "" = all */
    int  walk_min;
    int  max;
    int  poll_s;
    int  mock;
    int  tile_leave;         /* 1 = tile shows "weg over 6 min" (departure minus walk time) */
    char my_train[8];        /* "07:42": warn when this (planned) train is cancelled or late */
    int  warn_delay;         /* minutes of delay that count as late */
    char morning[16];        /* "06:30-09:00": next train as an Inbox message in this window */
} conf_t;

typedef struct {
    time_t planned, actual;
    int    delay_min;
    char   dest[64];
    char   cat[8];           /* IC, SPR, ICD ... */
    char   cat_long[32];
    char   via[96];
    char   track[8], planned_track[8];
    int    track_changed;
    int    cancelled;
    char   msg[120];
} train_t;

static conf_t g_conf;
static long   g_conf_mtime = -1;
static char   g_err[120];
static time_t g_updated;
static int    g_is_mock;
static train_t g_trains[MAX_TRAINS];
static int    g_n;

/* ---- config ------------------------------------------------------------- */

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
    ft_write_atomic(CONF_PATH, out, o, 1);
    free(out);
    free(t);
    /* g_conf_mtime is left alone: the reload this causes also picks up a UI
     * edit made while we were looking the station up */
}

static int load_conf(void) {
    long mt = ft_mtime(CONF_PATH);
    if (mt == g_conf_mtime) return 0;
    g_conf_mtime = mt;
    char * t = ft_read_file(CONF_PATH, NULL);
    conf_t c;
    memset(&c, 0, sizeof c);
    c.max = 8;
    c.poll_s = 60;
    c.warn_delay = 5;
    if (t) {
        char v[32];
        ft_conf_get(t, "api_key", c.api_key, sizeof c.api_key);
        ft_conf_get(t, "station", c.station, sizeof c.station);
        ft_conf_get(t, "station_code", c.station_code, sizeof c.station_code);
        ft_conf_get(t, "station_name", c.station_name, sizeof c.station_name);
        ft_conf_get(t, "direction", c.direction, sizeof c.direction);
        if (ft_conf_get(t, "walk_min", v, sizeof v)) c.walk_min = atoi(v);
        if (ft_conf_get(t, "max", v, sizeof v)) c.max = atoi(v);
        if (ft_conf_get(t, "poll", v, sizeof v)) c.poll_s = atoi(v);
        if (ft_conf_get(t, "mock", v, sizeof v)) c.mock = atoi(v);
        if (ft_conf_get(t, "tile_leave", v, sizeof v)) c.tile_leave = atoi(v) == 1;
        ft_conf_get(t, "my_train", c.my_train, sizeof c.my_train);
        if (ft_conf_get(t, "warn_delay", v, sizeof v)) c.warn_delay = atoi(v);
        ft_conf_get(t, "morning", c.morning, sizeof c.morning);
        free(t);
    } else {
        const char * def =
            "# NS departures for FreeToon. Edit on the Toon (tile > settings) or here.\n"
            "# api_key: apiportal.ns.nl, product \"Ns-App\" (primary key)\n"
            "api_key=\n"
            "# station: a name (\"Utrecht Centraal\") or code (\"UT\"); code/name are filled in\n"
            "station=\n"
            "station_code=\n"
            "station_name=\n"
            "# direction: only trains to/via these places, comma-separated (empty = all)\n"
            "direction=\n"
            "# walk_min: hide trains you can't catch anymore\n"
            "walk_min=0\n"
            "max=8\n"
            "poll=60\n"
            "# mock=1 shows sample departures (also used while api_key is empty)\n"
            "mock=0\n"
            "# options, all off by default:\n"
            "# tile_leave=1: tile shows when to leave (departure minus walk_min)\n"
            "tile_leave=0\n"
            "# my_train=07:42: Inbox warning when that train is cancelled or warn_delay+ min late\n"
            "my_train=\n"
            "warn_delay=5\n"
            "# morning=06:30-09:00: the next train as an Inbox message in that window\n"
            "morning=\n";
        ft_write_atomic(CONF_PATH, def, strlen(def), 1);
        g_conf_mtime = ft_mtime(CONF_PATH);
    }
    if (c.max < 1) c.max = 1;
    if (c.max > MAX_TRAINS) c.max = MAX_TRAINS;
    if (c.poll_s < 30) c.poll_s = 30;
    if (c.walk_min < 0) c.walk_min = 0;
    if (c.walk_min > 60) c.walk_min = 60;
    if (c.warn_delay < 1) c.warn_delay = 1;
    int h, m, h2, m2;
    if (sscanf(c.my_train, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60)
        snprintf(c.my_train, sizeof c.my_train, "%02d:%02d", h, m);
    else c.my_train[0] = 0;
    if (sscanf(c.morning, "%d:%d-%d:%d", &h, &m, &h2, &m2) == 4 && h < 24 && h2 <= 24 && m < 60 && m2 < 60)
        snprintf(c.morning, sizeof c.morning, "%02d:%02d-%02d:%02d", h, m, h2, m2);
    else c.morning[0] = 0;
    g_conf = c;
    ftlog("config: station=%s (%s) direction=%s walk=%d%s%s", c.station[0] ? c.station : "(none)",
          c.station_code[0] ? c.station_code : "?", c.direction[0] ? c.direction : "(all)", c.walk_min,
          c.api_key[0] ? " (api key set)" : " (no api key)", c.mock ? " mock" : "");
    return 1;
}

/* ---- helpers --------------------------------------------------------------- */

static void url_encode(const char * in, char * out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char * p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

/* "2026-11-18T10:14:00+0100" -> unix time */
static time_t parse_ns_time(const char * s) {
    if (!s || !*s) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int Y, M, D, h, m, sec = 0;
    char sign = '+';
    int oh = 0, om = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5) return 0;
    size_t sl = strlen(s);
    const char * z = sl >= 19 ? s + 19 : s + sl;
    if (*z == '.') while (*z && *z != '+' && *z != '-' && *z != 'Z') z++;
    if (*z == '+' || *z == '-') { sign = *z; sscanf(z + 1, "%2d%*[:]%2d", &oh, &om); if (!om && strlen(z) >= 5) sscanf(z + 1, "%2d%2d", &oh, &om); }
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
    time_t t = timegm(&tm);
    int off = (oh * 60 + om) * 60;
    return sign == '-' ? t + off : t - off;
}

static int ci_contains(const char * hay, const char * needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char * p = hay; *p; p++) if (!strncasecmp(p, needle, nl)) return 1;
    return 0;
}

/* direction filter: any comma-separated alternative in the destination or route */
static int matches_direction(const jv * d) {
    if (!g_conf.direction[0]) return 1;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", g_conf.direction);
    int any = 0;
    for (char * tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        size_t n = strlen(tok);
        while (n > 0 && tok[n - 1] == ' ') tok[--n] = 0;
        if (!n) continue;
        any = 1;
        if (ci_contains(jstr(d, "direction", ""), tok)) return 1;
        for (jv * r = jfirst(jget(d, "routeStations")); r; r = r->next)
            if (ci_contains(jstr(r, "mediumName", ""), tok)) return 1;
    }
    return !any;                     /* only separators ("," or " , "): no filter */
}

/* ---- mock answer (NS v2 format, around now) ------------------------------- */

static char * mock_departures(void) {
    static const struct { int in; const char * dest; const char * cat; const char * catl; const char * via;
                          int delay; const char * pt; const char * at; int cancel; const char * msg; } M[] = {
        {  4, "Amsterdam Centraal", "IC",  "Intercity", "Amsterdam Amstel",   2, "5",  "7",  0, "" },
        {  9, "Rotterdam Centraal", "IC",  "Intercity", "Gouda",              0, "18", "18", 0, "" },
        { 12, "Zwolle",             "IC",  "Intercity", "Amersfoort Centraal",0, "11", "11", 0, "" },
        { 16, "Den Haag Centraal",  "SPR", "Sprinter",  "Woerden",            0, "12", "12", 1,
              "Rijdt niet door een seinstoring" },
        { 19, "Nijmegen",           "IC",  "Intercity", "'s-Hertogenbosch",   5, "14", "14", 0, "" },
        { 24, "Amsterdam Centraal", "SPR", "Sprinter",  "Breukelen",          0, "4",  "4",  0, "" },
        { 31, "Schiphol Airport",   "IC",  "Intercity", "Amsterdam Zuid",     0, "7",  "7",  0, "" },
        { 34, "Arnhem Centraal",    "ICE", "ICE International", "Ede-Wageningen", 1, "19", "19", 0, "" },
        { 38, "Breda",              "SPR", "Sprinter",  "Geldermalsen",       0, "15", "15", 0, "" },
        { 46, "Amsterdam Centraal", "IC",  "Intercity", "Amsterdam Amstel",   0, "5",  "5",  0, "" },
    };
    time_t now = time(NULL);
    time_t base = now - now % 60;
    size_t cap = 16384, len = 0;
    char * out = (char *)malloc(cap);
    if (!out) return NULL;
    len += (size_t)snprintf(out + len, cap - len, "{\"payload\":{\"source\":\"MOCK\",\"departures\":[");
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        time_t p = base + M[i].in * 60, a = p + M[i].delay * 60;
        struct tm tp, ta;
        localtime_r(&p, &tp);
        localtime_r(&a, &ta);
        char ps[32], as[32];
        strftime(ps, sizeof ps, "%Y-%m-%dT%H:%M:%S%z", &tp);
        strftime(as, sizeof as, "%Y-%m-%dT%H:%M:%S%z", &ta);
        if (len + 1024 > cap) break;
        len += (size_t)snprintf(out + len, cap - len,
            "%s{\"direction\":\"%s\",\"name\":\"NS %d\",\"plannedDateTime\":\"%s\",\"actualDateTime\":\"%s\","
            "\"plannedTrack\":\"%s\",\"actualTrack\":\"%s\",\"product\":{\"categoryCode\":\"%s\",\"longCategoryName\":\"%s\"},"
            "\"trainCategory\":\"%s\",\"cancelled\":%s,\"routeStations\":[{\"mediumName\":\"%s\"}],"
            "\"messages\":[%s%s%s]}",
            i ? "," : "", M[i].dest, 3000 + (int)i * 17, ps, as, M[i].pt, M[i].at, M[i].cat, M[i].catl, M[i].cat,
            M[i].cancel ? "true" : "false", M[i].via,
            M[i].msg[0] ? "{\"message\":\"" : "", M[i].msg, M[i].msg[0] ? "\",\"style\":\"WARNING\"}" : "");
    }
    snprintf(out + len, cap - len, "]}}");
    return out;
}

/* ---- fetch + parse ---------------------------------------------------------- */

/* Name -> station code via the NS stations API. 1 when resolved. */
static int resolve_station(void) {
    const char * q = g_conf.station;
    if (!q[0]) return 0;
    char enc[160], url[300], hdr[128];
    url_encode(q, enc, sizeof enc);
    snprintf(hdr, sizeof hdr, "Ocp-Apim-Subscription-Key: %s", g_conf.api_key);
    const char * headers[] = { hdr, NULL };
    char * body = NULL;
    /* v2 stations search; answer: payload[{code, namen:{lang,middel,kort}}] */
    snprintf(url, sizeof url, API_BASE "/reisinformatie-api/api/v2/stations?q=%s&limit=5", enc);
    int code = ft_http_get(url, headers, 15, &body, 512 * 1024);
    jv * r = code == 200 ? jparse(body) : NULL;
    jv * best = NULL;
    for (jv * s = jfirst(jget(r, "payload")); s; s = s->next) {
        if (!best) best = s;
        if (!strcasecmp(jstr(s, "namen.lang", ""), q) || !strcasecmp(jstr(s, "code", ""), q)) { best = s; break; }
    }
    int ok = 0;
    if (best && jstr(best, "code", "")[0]) {
        snprintf(g_conf.station_code, sizeof g_conf.station_code, "%s", jstr(best, "code", ""));
        snprintf(g_conf.station_name, sizeof g_conf.station_name, "%s", jstr(best, "namen.lang", q));
        ok = 1;
    } else if (strlen(q) <= 5 && code == 200) {
        /* no match: try it as a station code ("UT", "ASD") */
        snprintf(g_conf.station_code, sizeof g_conf.station_code, "%.15s", q);
        for (char * p = g_conf.station_code; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(g_conf.station_name, sizeof g_conf.station_name, "%s", q);
        ok = 1;
    }
    if (ok) {
        ftlog("station '%s' -> %s (%s)", q, g_conf.station_code, g_conf.station_name);
        conf_write_key("station_code", g_conf.station_code);
        conf_write_key("station_name", g_conf.station_name);
    } else {
        snprintf(g_err, sizeof g_err, code == 401 || code == 403 ? "%s" : "%s",
                 code == 401 || code == 403 ? FT_TR("NS-sleutel geweigerd", "NS key refused")
                                            : FT_TR("Station niet gevonden", "Station not found"));
        ftlog("station lookup for '%s' failed (HTTP %d)", q, code);
    }
    jfree(r);
    free(body);
    return ok;
}

static void parse_departures(const char * body) {
    jv * r = jparse(body);
    jv * deps = jpath(r, "payload.departures");
    time_t now = time(NULL), catch_from = now + g_conf.walk_min * 60;
    g_n = 0;
    for (jv * d = jfirst(deps); d && g_n < g_conf.max; d = d->next) {
        if (!matches_direction(d)) continue;
        train_t t;
        memset(&t, 0, sizeof t);
        t.planned = parse_ns_time(jstr(d, "plannedDateTime", ""));
        t.actual = parse_ns_time(jstr(d, "actualDateTime", ""));
        if (!t.actual) t.actual = t.planned;
        if (t.actual < catch_from && !(g_conf.walk_min == 0 && t.actual >= now - 60)) continue;
        t.delay_min = t.planned && t.actual > t.planned ? (int)((t.actual - t.planned + 30) / 60) : 0;
        snprintf(t.dest, sizeof t.dest, "%s", jstr(d, "direction", "?"));
        const char * cat = jstr(d, "product.categoryCode", jstr(d, "trainCategory", ""));
        snprintf(t.cat, sizeof t.cat, "%s", cat);
        snprintf(t.cat_long, sizeof t.cat_long, "%s", jstr(d, "product.longCategoryName", cat));
        int nv = 0;
        for (jv * rs = jfirst(jget(d, "routeStations")); rs && nv < 2; rs = rs->next, nv++) {
            size_t l = strlen(t.via);
            snprintf(t.via + l, sizeof t.via - l, "%s%s", nv ? ", " : "", jstr(rs, "mediumName", ""));
        }
        snprintf(t.planned_track, sizeof t.planned_track, "%s", jstr(d, "plannedTrack", ""));
        snprintf(t.track, sizeof t.track, "%s", jstr(d, "actualTrack", t.planned_track));
        t.track_changed = t.planned_track[0] && strcmp(t.track, t.planned_track) != 0;
        t.cancelled = jbool(d, "cancelled", 0);
        jv * m = jfirst(jget(d, "messages"));
        if (m) snprintf(t.msg, sizeof t.msg, "%s", jstr(m, "message", ""));
        g_trains[g_n++] = t;
    }
    jfree(r);
}

static void fetch(void) {
    g_err[0] = 0;
    int use_mock = g_conf.mock || !g_conf.api_key[0];
    if (!g_conf.station[0] && !use_mock) { snprintf(g_err, sizeof g_err, "%s", FT_TR("Kies een station", "Choose a station")); g_n = 0; return; }
    char * body = NULL;
    if (use_mock) {
        body = mock_departures();
        if (!g_conf.station_name[0]) snprintf(g_conf.station_name, sizeof g_conf.station_name, "%s",
                                              g_conf.station[0] ? g_conf.station : "Utrecht Centraal");
    } else {
        if (!g_conf.station_code[0] && !resolve_station()) { g_n = 0; return; }
        char enc[40], url[300], hdr[128];
        url_encode(g_conf.station_code, enc, sizeof enc);
        snprintf(url, sizeof url, API_BASE "/reisinformatie-api/api/v2/departures?station=%s&maxJourneys=40", enc);
        snprintf(hdr, sizeof hdr, "Ocp-Apim-Subscription-Key: %s", g_conf.api_key);
        const char * headers[] = { hdr, "Accept: application/json", NULL };
        int code = ft_http_get(url, headers, 15, &body, 1024 * 1024);
        if (code != 200) {
            if (code == 0) snprintf(g_err, sizeof g_err, "%s", FT_TR("NS niet bereikbaar", "NS unreachable"));
            else if (code == 401 || code == 403) snprintf(g_err, sizeof g_err, "%s", FT_TR("NS-sleutel geweigerd", "NS key refused"));
            else if (code == 400 || code == 404) snprintf(g_err, sizeof g_err, "%s", FT_TR("Onbekend station", "Unknown station"));
            else if (code == 429) snprintf(g_err, sizeof g_err, "%s", FT_TR("Te veel verzoeken bij NS", "Too many NS requests"));
            else snprintf(g_err, sizeof g_err, "NS HTTP %d", code);
            ftlog("departures for %s: HTTP %d", g_conf.station_code, code);
            free(body);
            return;                       /* keep the last list on a hiccup */
        }
    }
    g_is_mock = use_mock;
    parse_departures(body);
    g_updated = time(NULL);
    free(body);
}

/* ---- publish ----------------------------------------------------------------- */

static void hm(time_t t, char * out, size_t n) {
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void publish(void) {
    int en = ft_english();
    time_t now = time(NULL);
    /* drop trains that left (or can't be caught anymore) while we wait for the next poll */
    int keep = 0;
    time_t catch_from = g_conf.walk_min ? now + g_conf.walk_min * 60 : now - 30;
    for (int i = 0; i < g_n; i++) if (g_trains[i].actual >= catch_from) g_trains[keep++] = g_trains[i];
    g_n = keep;

    char value[32], subtitle[128], alert[160] = "";
    const train_t * next = NULL;
    for (int i = 0; i < g_n && !next; i++) if (!g_trains[i].cancelled) next = &g_trains[i];
    long leave = next ? (long)((next->actual - now) / 60) - g_conf.walk_min : 0;
    if (next) {
        char at[8];
        hm(next->actual, at, sizeof at);
        char d[16] = "";
        if (next->delay_min > 0) snprintf(d, sizeof d, "+%d  ", next->delay_min);
        if (g_conf.tile_leave) {
            if (leave <= 0) snprintf(value, sizeof value, "%s", en ? "Leave now" : "Nu weg");
            else snprintf(value, sizeof value, en ? "%ld min" : "%ld min", leave);
            snprintf(subtitle, sizeof subtitle, en ? "leave for the %s %s %s" : "weg voor de %s %s %s", at, next->cat, next->dest);
        } else {
            snprintf(value, sizeof value, "%s", at);
            snprintf(subtitle, sizeof subtitle, "%s%s %s - %s %s", d, next->cat, next->dest, en ? "track" : "spoor", next->track);
        }
    } else {
        snprintf(value, sizeof value, "--");
        snprintf(subtitle, sizeof subtitle, "%s", g_err[0] ? g_err : (en ? "no trains" : "geen treinen"));
    }

    /* Inbox: warning for your own train first, else the morning line. */
    if (g_conf.my_train[0]) {
        for (int i = 0; i < g_n; i++) {
            char ps[8];
            hm(g_trains[i].planned, ps, sizeof ps);
            if (strcmp(ps, g_conf.my_train) != 0) continue;
            const train_t * t = &g_trains[i];
            if (t->cancelled)
                snprintf(alert, sizeof alert, en ? "Note: your %s train to %s is cancelled" : "Let op: je trein van %s naar %s rijdt niet",
                         ps, t->dest);
            else if (t->delay_min >= g_conf.warn_delay) {
                char as[8];
                hm(t->actual, as, sizeof as);
                snprintf(alert, sizeof alert, en ? "Note: your %s train to %s is %d min late (%s)"
                                                 : "Let op: je trein van %s naar %s is %d min later (%s)",
                         ps, t->dest, t->delay_min, as);
            }
            break;
        }
    }
    int fh, fm, th, tm_;
    if (!alert[0] && next && sscanf(g_conf.morning, "%d:%d-%d:%d", &fh, &fm, &th, &tm_) == 4) {
        struct tm lt;
        localtime_r(&now, &lt);
        int cur = lt.tm_hour * 60 + lt.tm_min, from = fh * 60 + fm, to = th * 60 + tm_;
        if (cur >= from && cur < to) {
            char at[8];
            hm(next->actual, at, sizeof at);
            if (leave > 0)
                snprintf(alert, sizeof alert, en ? "Leave in %ld min: %s %s to %s, track %s" : "Vertrek over %ld min: %s %s naar %s, spoor %s",
                         leave, at, next->cat, next->dest, next->track);
            else
                snprintf(alert, sizeof alert, en ? "Train %s %s to %s, track %s" : "Trein %s %s naar %s, spoor %s",
                         at, next->cat, next->dest, next->track);
        }
    }
    ftapp_tile(value, subtitle, alert);

    size_t cap = 16384, len = 0;
    char * b = (char *)malloc(cap);
    if (!b) return;
#define APP(...) do { int r_ = snprintf(b + len, cap - len, __VA_ARGS__); if (r_ > 0 && len + (size_t)r_ < cap) len += (size_t)r_; } while (0)
#define STR(v) do { int r_ = jesc(b, cap, len, v); if (r_ > 0) len = (size_t)r_; } while (0)
    APP("{\"updated\":%ld,\"mock\":%d,\"error\":", (long)g_updated, g_is_mock);
    STR(g_err);
    APP(",\"station\":");
    STR(g_conf.station_name[0] ? g_conf.station_name : g_conf.station);
    APP(",\"direction\":");
    STR(g_conf.direction);
    APP(",\"walk_min\":%d,\"alert\":", g_conf.walk_min);
    STR(alert);
    APP(",\"trains\":[");
    for (int i = 0; i < g_n; i++) {
        const train_t * t = &g_trains[i];
        char ts[8], ps[8];
        hm(t->actual, ts, sizeof ts);
        hm(t->planned, ps, sizeof ps);
        APP("%s{\"time\":\"%s\",\"planned\":\"%s\",\"in_min\":%ld,\"delay\":%d,\"dest\":", i ? "," : "", ts, ps,
            (long)((t->actual - now) / 60), t->delay_min);
        STR(t->dest);
        APP(",\"cat\":"); STR(t->cat);
        APP(",\"cat_long\":"); STR(t->cat_long);
        APP(",\"via\":"); STR(t->via);
        APP(",\"track\":"); STR(t->track);
        APP(",\"planned_track\":"); STR(t->planned_track);
        APP(",\"track_changed\":%d,\"cancelled\":%d,\"msg\":", t->track_changed, t->cancelled);
        STR(t->msg);
        APP("}");
    }
    APP("]}\n");
#undef APP
#undef STR
    ft_write_atomic(STATE_PATH, b, len, 0);
    free(b);
}

/* ---- main -------------------------------------------------------------------- */

int main(int argc, char ** argv) {
    int once = argc > 1 && !strcmp(argv[1], "--once");
    ftapp_init();
    load_conf();
    if (once) { fetch(); publish(); return g_err[0] ? 1 : 0; }
    time_t last_fetch = 0, last_pub = 0;
    char last_station[64] = "";
    while (ftapp_running) {
        if (load_conf()) {
            /* station text changed in the UI: forget the old code so it is looked up again */
            if (strcmp(last_station, g_conf.station) != 0) {
                if (last_station[0] || !g_conf.station_code[0]) {
                    g_conf.station_code[0] = 0;
                    g_conf.station_name[0] = 0;
                }
                snprintf(last_station, sizeof last_station, "%s", g_conf.station);
            }
            last_fetch = 0;
        }
        ftapp_bxt_service();
        char * cmd;
        while ((cmd = ftapp_next_cmd())) { free(cmd); last_fetch = 0; }   /* {"cmd":"refresh"} */
        time_t now = time(NULL);
        if (now - last_fetch >= g_conf.poll_s) { last_fetch = now; fetch(); publish(); last_pub = now; }
        if (now - last_pub >= 15) { publish(); last_pub = now; }
        usleep(500 * 1000);
    }
    ftlog("stopping");
    return 0;
}
