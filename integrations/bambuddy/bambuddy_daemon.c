/*
 * bambuddy_daemon.c - FreeToon integration: 3D-printer status from BamBuddy.
 *
 * Two ways to get the printer status, both may be on at once:
 *
 *   pull  The Toon asks BamBuddy itself every `poll` seconds:
 *         GET <url>/api/v1/printers/<printer_id>/status (header X-API-Key),
 *         and the plate thumbnail from the status' cover_url when a new print
 *         starts. Use this when the Toon can reach BamBuddy.
 *
 *   push  The daemon listens on `push_port` (default 8766) for
 *           POST /status   the same JSON BamBuddy's status endpoint returns
 *           POST /cover    the PNG thumbnail
 *         sent by a relay that can reach both (relay/bambuddy-relay.sh, e.g. on
 *         the NAS when the Toon sits in an IoT network that can't reach it).
 *         The reply to /status says {"need_cover":true} when the Toon has no
 *         thumbnail for the current print yet. Optional shared secret:
 *         `push_token`, sent by the relay as header X-Toon-Token.
 *
 * Output:
 *   - tile: <value> (63% / Klaar / Pauze / Vrij / Offline) and <subtitle>
 *     (time left and file name); <alert> raises an Inbox notification when a
 *     print finishes or fails, and clears when the next one starts;
 *   - /var/volatile/tmp/bambuddy/state.json: normalised status for the UI
 *     plugin (bambuddy_ui.<toon>.so), and cover.png next to it.
 *
 * Settings: /mnt/data/integrations/bambuddy/bambuddy.conf (key=value),
 * written by the UI plugin; re-read when it changes.
 */
#define _GNU_SOURCE
#define FTAPP_ID "bambuddy"
#include "ftapp.h"
#include "ftjson.h"

#include <math.h>

#define CONF_PATH   FTAPP_DIR "/bambuddy.conf"
#define STATE_PATH  FTAPP_RUN "/state.json"
#define COVER_PATH  FTAPP_RUN "/cover.png"
#define STALE_S     120          /* no status for this long = offline */
#define MAX_STATUS  (64 * 1024)       /* a printer status is ~10 KB */
#define MAX_COVER   (512 * 1024)
#define MAX_COVER_PX 1024             /* toonui decodes the PNG: keep it small */

typedef struct {
    char url[160];
    char api_key[96];
    int  printer_id;
    int  poll_s;
    int  push_port;
    char push_token[64];
    char push_from[48];       /* only accept pushes from this IPv4 address (empty = any) */
    int  notify;              /* 1 = Inbox notification when a print finishes or fails */
    int  tile_eta;            /* 1 = tile subtitle "klaar om 16:42" instead of "nog 2:14" */
} conf_t;

static conf_t g_conf;
static long   g_conf_mtime = -1;

/* last status we got (from pull or push) */
static char * g_status_json;
static time_t g_status_at;
static char   g_status_src[8];
#define FILE_N 160
static char   g_cover_for[FILE_N];   /* subtask the cover.png belongs to */
static char   g_last_state[24];
static char   g_alert[160];
static char   g_err[120];

static void load_conf(void) {
    long mt = ft_mtime(CONF_PATH);
    if (mt == g_conf_mtime) return;
    g_conf_mtime = mt;
    char * t = ft_read_file(CONF_PATH, NULL);
    conf_t c;
    memset(&c, 0, sizeof c);
    c.printer_id = 0;
    c.poll_s = 15;
    c.push_port = 0;                  /* push is opt-in */
    if (t) {
        char v[32];
        ft_conf_get(t, "url", c.url, sizeof c.url);
        ft_conf_get(t, "api_key", c.api_key, sizeof c.api_key);
        ft_conf_get(t, "push_token", c.push_token, sizeof c.push_token);
        ft_conf_get(t, "push_from", c.push_from, sizeof c.push_from);
        if (ft_conf_get(t, "printer_id", v, sizeof v)) c.printer_id = atoi(v);
        if (ft_conf_get(t, "poll", v, sizeof v)) c.poll_s = atoi(v);
        if (ft_conf_get(t, "push_port", v, sizeof v)) c.push_port = atoi(v);
        if (ft_conf_get(t, "notify", v, sizeof v)) c.notify = atoi(v) == 1;
        if (ft_conf_get(t, "tile_eta", v, sizeof v)) c.tile_eta = atoi(v) == 1;
        free(t);
    } else {
        /* first start: write a commented default the UI and users can edit */
        const char * def =
            "# BamBuddy for FreeToon. Edit on the Toon (tile > settings) or here.\n"
            "# url: BamBuddy base URL the Toon can reach, e.g. http://192.168.1.20:8000\n"
            "#      (empty = only receive pushes from a relay on push_port)\n"
            "url=\n"
            "api_key=\n"
            "# 0 = first printer\n"
            "printer_id=0\n"
            "poll=15\n"
            "# push from a relay: port (e.g. 8766; 0 = off), shared secret (required), relay address (empty = any)\n"
            "push_port=0\n"
            "push_token=\n"
            "push_from=\n"
            "# options (1 = on): Inbox notification when a print is done or fails;\n"
            "# finish time on the tile instead of the time left\n"
            "notify=0\n"
            "tile_eta=0\n";
        ft_write_atomic(CONF_PATH, def, strlen(def), 1);
        g_conf_mtime = ft_mtime(CONF_PATH);
    }
    size_t ul = strlen(c.url);
    while (ul > 0 && c.url[ul - 1] == '/') c.url[--ul] = 0;
    if (c.poll_s < 5) c.poll_s = 5;
    if (c.push_port < 0 || c.push_port > 65535) c.push_port = 0;
    /* ports the Toon itself uses (ssh, web, BoxTalk, vnc, toonui) */
    if (c.push_port == 22 || c.push_port == 80 || c.push_port == 1337 || c.push_port == 5900 || c.push_port == 10081 ||
        c.push_port == 10080) {
        ftlog("push_port %d is used by the Toon itself, push stays off", c.push_port);
        c.push_port = 0;
    }
    if (c.push_port && strlen(c.push_token) < 8) {
        ftlog("push needs push_token (8+ characters); push stays off");
        c.push_port = 0;
    }
    struct in_addr ia;
    if (c.push_from[0] && inet_pton(AF_INET, c.push_from, &ia) != 1) {
        ftlog("push_from '%s' is not an IPv4 address, ignoring it", c.push_from);
        c.push_from[0] = 0;
    }
    g_conf = c;
    ftlog("config: url=%s printer=%d poll=%ds push_port=%d%s", c.url[0] ? c.url : "(none)", c.printer_id,
          c.poll_s, c.push_port, c.api_key[0] ? " (api key set)" : "");
}

/* ---- status -> state.json + tile ---------------------------------------- */

static int is_url_safe(const char * s) {
    for (; *s; s++) if ((unsigned char)*s <= ' ' || *s == '"' || *s == '\\') return 0;
    return 1;
}

static const char * state_label(const char * st, int en) {
    if (!strcmp(st, "RUNNING")) return en ? "Printing" : "Bezig";
    if (!strcmp(st, "PREPARE") || !strcmp(st, "SLICING") || !strcmp(st, "INIT")) return en ? "Preparing" : "Voorbereiden";
    if (!strcmp(st, "PAUSE")) return en ? "Paused" : "Gepauzeerd";
    if (!strcmp(st, "FINISH")) return en ? "Finished" : "Klaar";
    if (!strcmp(st, "FAILED")) return en ? "Failed" : "Mislukt";
    if (!strcmp(st, "IDLE")) return en ? "Idle" : "Vrij";
    if (!strcmp(st, "OFFLINE")) return "Offline";
    return st[0] ? st : "?";
}

static void fmt_left(int minutes, char * out, size_t n, int en) {
    if (minutes <= 0) { snprintf(out, n, "%s", en ? "almost done" : "bijna klaar"); return; }
    if (minutes < 60) snprintf(out, n, en ? "%d min left" : "nog %d min", minutes);
    else snprintf(out, n, en ? "%d:%02d left" : "nog %d:%02d", minutes / 60, minutes % 60);
}

/* Strip ".gcode.3mf" / ".3mf" / ".gcode" for display. */
static void clean_name(const char * in, char * out, size_t n) {
    snprintf(out, n, "%s", in ? in : "");
    static const char * ext[] = { ".gcode.3mf", ".3mf", ".gcode" };
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(out), el = strlen(ext[i]);
        if (l > el && !strcasecmp(out + l - el, ext[i])) { out[l - el] = 0; break; }
    }
}

static void publish(void) {
    int en = ft_english();
    time_t now = time(NULL);
    jv * s = g_status_json ? jparse(g_status_json) : NULL;
    int fresh = s && now - g_status_at <= STALE_S;
    int connected = fresh && jbool(s, "connected", 0);
    const char * st = fresh ? jstr(s, "state", "") : "";
    if (fresh && !connected) st = "OFFLINE";
    char file[FILE_N];
    const char * sub = jstr(s, "subtask_name", NULL);
    clean_name(sub && *sub ? sub : jstr(s, "current_print", ""), file, sizeof file);
    double progress = jnum(s, "progress", 0);
    int remaining = (int)jnum(s, "remaining_time", 0);      /* minutes */
    int plate_clear = jbool(s, "awaiting_plate_clear", 0);
    int hms = jcount(jget(s, "hms_errors"));

    /* ---- tile + alert ---- */
    char value[48], subtitle[128], left[40];
    if (!fresh) {
        snprintf(value, sizeof value, "Offline");
        snprintf(subtitle, sizeof subtitle, "%s", g_err[0] ? g_err : (en ? "no data from BamBuddy" : "geen gegevens van BamBuddy"));
    } else if (!strcmp(st, "RUNNING")) {
        snprintf(value, sizeof value, "%d%%", (int)floor(progress));
        fmt_left(remaining, left, sizeof left, en);
        if (g_conf.tile_eta && remaining > 0) {
            time_t eta = now + (time_t)remaining * 60;
            struct tm tm;
            localtime_r(&eta, &tm);
            snprintf(left, sizeof left, en ? "done at %02d:%02d" : "klaar om %02d:%02d", tm.tm_hour, tm.tm_min);
        }
        snprintf(subtitle, sizeof subtitle, "%s - %s", left, file);
    } else if (!strcmp(st, "PAUSE")) {
        snprintf(value, sizeof value, "%s", en ? "Paused" : "Pauze");
        snprintf(subtitle, sizeof subtitle, "%d%% - %s", (int)floor(progress), file);
    } else if (!strcmp(st, "FINISH")) {
        snprintf(value, sizeof value, "%s", state_label(st, en));
        snprintf(subtitle, sizeof subtitle, "%s%s", plate_clear ? (en ? "clear plate - " : "plaat leegmaken - ") : "", file);
    } else {
        snprintf(value, sizeof value, "%s", state_label(st, en));
        snprintf(subtitle, sizeof subtitle, "%s", !strcmp(st, "FAILED") ? file : jstr(s, "name", "BamBuddy"));
    }

    /* OFFLINE is a connection blip, not a print state: keep the last real one,
     * so RUNNING -> (offline) -> FINISH still raises the "done" alert. */
    if (fresh && strcmp(st, "OFFLINE") != 0 && strcmp(st, g_last_state) != 0) {
        if (!strcmp(st, "FINISH") && !strcmp(g_last_state, "RUNNING"))
            snprintf(g_alert, sizeof g_alert, en ? "3D print finished: %s" : "3D-print klaar: %s", file);
        else if (!strcmp(st, "FAILED") && g_last_state[0])
            snprintf(g_alert, sizeof g_alert, en ? "3D print failed: %s" : "3D-print mislukt: %s", file);
        else if (!strcmp(st, "RUNNING") || !strcmp(st, "PREPARE") || !strcmp(st, "IDLE"))
            g_alert[0] = 0;
        if (g_last_state[0] || !strcmp(st, "RUNNING")) ftlog("state %s -> %s", g_last_state[0] ? g_last_state : "-", st);
        snprintf(g_last_state, sizeof g_last_state, "%s", st);
    }
    ftapp_tile(value, subtitle, g_conf.notify ? g_alert : "");

    /* ---- state.json for the UI plugin ---- */
    char buf[4096];
    int len = 0;
#define APP(...) do { int r_ = snprintf(buf + len, sizeof buf - (size_t)len, __VA_ARGS__); \
                      if (r_ > 0 && len + r_ < (int)sizeof buf) len += r_; } while (0)
#define APPS(k, v) do { APP("\"%s\":", k); int r_ = jesc(buf, sizeof buf, (size_t)len, v); if (r_ > 0) len = r_; APP(","); } while (0)
    APP("{");
    APP("\"updated\":%ld,\"fresh\":%d,", (long)g_status_at, fresh);
    APPS("source", g_status_src);
    APPS("error", fresh ? "" : g_err);
    APPS("printer", jstr(s, "name", ""));
    APPS("state", st);
    APPS("state_label", fresh ? state_label(st, en) : "Offline");
    APPS("file", file);
    APP("\"progress\":%.1f,\"remaining_min\":%d,", progress, remaining);
    APP("\"layer\":%d,\"total_layers\":%d,", (int)jnum(s, "layer_num", 0), (int)jnum(s, "total_layers", 0));
    jv * t = jget(s, "temperatures");
    APP("\"nozzle\":%.0f,\"nozzle_target\":%.0f,", jnum(t, "nozzle", -999), jnum(t, "nozzle_target", 0));
    APP("\"nozzle2\":%.0f,\"nozzle2_target\":%.0f,", jnum(t, "nozzle_2", -999), jnum(t, "nozzle_2_target", 0));
    APP("\"bed\":%.0f,\"bed_target\":%.0f,", jnum(t, "bed", -999), jnum(t, "bed_target", 0));
    APP("\"chamber\":%.0f,\"chamber_target\":%.0f,", jnum(t, "chamber", -999), jnum(t, "chamber_target", 0));
    APP("\"plate_clear\":%d,\"hms\":%d,", plate_clear, hms);
    APPS("stage", jstr(s, "stg_cur_name", ""));
    int have_cover = access(COVER_PATH, R_OK) == 0 && g_cover_for[0] && !strcmp(g_cover_for, file);
    APP("\"cover\":%d,", have_cover);
    APP("\"ams\":[");
    int first = 1;
    for (jv * unit = jfirst(jget(s, "ams")); unit; unit = unit->next) {
        for (jv * tray = jfirst(jget(unit, "tray")); tray; tray = tray->next) {
            if (!jbool(tray, "exists", 1)) continue;
            const char * col = jstr(tray, "tray_color", "");
            const char * typ = jstr(tray, "tray_type", "");
            if (!typ[0] && !col[0]) continue;
            APP("%s{\"color\":", first ? "" : ",");
            int r_ = jesc(buf, sizeof buf, (size_t)len, col); if (r_ > 0) len = r_;
            APP(",\"type\":");
            r_ = jesc(buf, sizeof buf, (size_t)len, typ); if (r_ > 0) len = r_;
            APP(",\"remain\":%d}", (int)jnum(tray, "remain", -1));
            first = 0;
        }
    }
    APP("]}\n");
#undef APP
#undef APPS
    ft_write_atomic(STATE_PATH, buf, (size_t)len, 0);
    jfree(s);
}

/* Take a status JSON (from either path). Returns 1 if it parsed. */
static int accept_status(const char * body, const char * src) {
    jv * s = jparse(body);
    if (!s || s->type != JV_OBJ || !jget(s, "state")) { jfree(s); return 0; }
    jfree(s);
    free(g_status_json);
    g_status_json = strdup(body);
    g_status_at = time(NULL);
    snprintf(g_status_src, sizeof g_status_src, "%s", src);
    g_err[0] = 0;
    return 1;
}

/* 1 when buf (len bytes) is a PNG of at most MAX_COVER_PX x MAX_COVER_PX. */
static int png_ok(const unsigned char * b, size_t len) {
    if (len < 24 || memcmp(b, "\x89PNG\r\n\x1a\n", 8) != 0 || memcmp(b + 12, "IHDR", 4) != 0) return 0;
    unsigned w = (unsigned)b[16] << 24 | (unsigned)b[17] << 16 | (unsigned)b[18] << 8 | b[19];
    unsigned h = (unsigned)b[20] << 24 | (unsigned)b[21] << 16 | (unsigned)b[22] << 8 | b[23];
    return w > 0 && h > 0 && w <= MAX_COVER_PX && h <= MAX_COVER_PX;
}

static void current_file(char * out, size_t n) {
    out[0] = 0;
    jv * s = g_status_json ? jparse(g_status_json) : NULL;
    if (!s) return;
    const char * sub = jstr(s, "subtask_name", NULL);
    clean_name(sub && *sub ? sub : jstr(s, "current_print", ""), out, n);
    jfree(s);
}

/* ---- pull ------------------------------------------------------------------ */

static int g_printer_resolved;

static void pull_once(void) {
    if (!g_conf.url[0]) return;
    if (!is_url_safe(g_conf.url)) { snprintf(g_err, sizeof g_err, "url?"); return; }
    char hdr[128];
    snprintf(hdr, sizeof hdr, "X-API-Key: %s", g_conf.api_key);
    const char * headers[] = { hdr, "Accept: application/json", NULL };
    char url[300];
    char * body = NULL;

    int pid = g_conf.printer_id;
    if (pid <= 0) {
        if (!g_printer_resolved) {
            snprintf(url, sizeof url, "%s/api/v1/printers/", g_conf.url);
            int code = ft_http_get(url, g_conf.api_key[0] ? headers : headers + 1, 10, &body, 512 * 1024);
            jv * list = code == 200 ? jparse(body) : NULL;
            jv * p = jfirst(list);
            if (p) g_printer_resolved = (int)jnum(p, "id", 0);
            jfree(list);
            free(body);
            body = NULL;
            if (!g_printer_resolved) {
                if (code == 0) snprintf(g_err, sizeof g_err, "%s", ft_english() ? "BamBuddy unreachable" : "BamBuddy onbereikbaar");
                else if (code == 200) snprintf(g_err, sizeof g_err, "%s", ft_english() ? "BamBuddy: no printers" : "BamBuddy: geen printers");
                else snprintf(g_err, sizeof g_err, "BamBuddy HTTP %d", code);
                return;
            }
        }
        pid = g_printer_resolved;
    }
    snprintf(url, sizeof url, "%s/api/v1/printers/%d/status", g_conf.url, pid);
    int code = ft_http_get(url, g_conf.api_key[0] ? headers : headers + 1, 10, &body, 1024 * 1024);
    if (code == 200 && body && accept_status(body, "pull")) {
        /* thumbnail for a new print */
        jv * s = jparse(body);
        const char * cover = jstr(s, "cover_url", "");
        char file[FILE_N], fallback[64];
        current_file(file, sizeof file);
        /* cover_url is null once a print is done; /cover still has the last plate */
        snprintf(fallback, sizeof fallback, "/api/v1/printers/%d/cover", pid);
        if (cover[0] != '/') cover = fallback;
        if (is_url_safe(cover) && file[0] && strcmp(file, g_cover_for) != 0) {
            snprintf(url, sizeof url, "%s%s", g_conf.url, cover);
            char * png = NULL;
            char * argv[] = { "curl", "-s", "-f", "-m", "20", "--max-filesize", "524288", "-H", hdr,
                              "-o", COVER_PATH ".part", "--", url, NULL };
            if (!g_conf.api_key[0]) { argv[7] = "-s"; argv[8] = "-s"; }
            int rc = ftapp_run(argv, &png, NULL, 4096, 25);
            free(png);
            size_t plen = 0;
            char * pb = rc == 0 ? ft_read_file(COVER_PATH ".part", &plen) : NULL;
            int good = pb && png_ok((const unsigned char *)pb, plen);
            free(pb);
            if (!good) { if (rc == 0) ftlog("cover for %s is not a small PNG, ignored", file); rc = 1; }
            if (rc == 0 && rename(COVER_PATH ".part", COVER_PATH) == 0) {
                snprintf(g_cover_for, sizeof g_cover_for, "%s", file);
                ftlog("cover for %s fetched", file);
            } else {
                unlink(COVER_PATH ".part");
            }
        }
        jfree(s);
    } else if (code == 0) {
        snprintf(g_err, sizeof g_err, "%s", ft_english() ? "BamBuddy unreachable" : "BamBuddy onbereikbaar");
    } else if (code == 401 || code == 403) {
        snprintf(g_err, sizeof g_err, "%s", ft_english() ? "BamBuddy: API key refused" : "BamBuddy: API-sleutel geweigerd");
    } else {
        snprintf(g_err, sizeof g_err, "BamBuddy HTTP %d", code);
    }
    free(body);
}

/* ---- push (tiny HTTP server) ---------------------------------------------- */

static int    g_listen = -1;
static int    g_listen_port = -1;
static char   g_listen_from[48];
static int    g_fw_port;               /* the rule WE inserted (0 = none) */
static char   g_fw_from[48];
static time_t g_listen_try;

/* The stock Toon firewall (HCB-INPUT) drops every TCP port except 22, 80,
 * 5900 and 10081. Open only our push port, only while push is on, and only
 * for push_from when set. Runtime rule: gone after a reboot until the daemon
 * adds it again; removed when push is turned off or the daemon stops. */
static void fw_rule(const char * op, int port, const char * from) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    char * argv[16];
    int a = 0;
    argv[a++] = "/usr/sbin/iptables";
    argv[a++] = (char *)op;
    argv[a++] = "HCB-INPUT";
    if (!strcmp(op, "-I")) argv[a++] = "1";
    argv[a++] = "-p"; argv[a++] = "tcp"; argv[a++] = "--dport"; argv[a++] = ps;
    if (from && from[0]) { argv[a++] = "-s"; argv[a++] = (char *)from; }
    argv[a++] = "-j"; argv[a++] = "ACCEPT";
    argv[a] = NULL;
    char * out = NULL;
    int rc = ftapp_run(argv, &out, NULL, 4096, 5);
    free(out);
    if (!strcmp(op, "-C")) return;
    if (rc == 0) ftlog("firewall: %s port %d%s%s", !strcmp(op, "-I") ? "opened" : "closed", port,
                       from && from[0] ? " for " : "", from && from[0] ? from : "");
}

static int fw_has(int port, const char * from) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    char * argv[] = { "/usr/sbin/iptables", "-C", "HCB-INPUT", "-p", "tcp", "--dport", ps, from && from[0] ? "-s" : "-j",
                      from && from[0] ? (char *)from : "ACCEPT", from && from[0] ? "-j" : NULL, "ACCEPT", NULL };
    char * out = NULL;
    int rc = ftapp_run(argv, &out, NULL, 4096, 5);
    free(out);
    return rc == 0;
}

static void fw_open(int port, const char * from) {
    if (port <= 0) return;
    if (!fw_has(port, from)) fw_rule("-I", port, from);
    g_fw_port = port;
    snprintf(g_fw_from, sizeof g_fw_from, "%s", from);
}
/* Only ever removes the rule this daemon inserted. */
static void fw_close(void) {
    if (g_fw_port <= 0) return;
    if (fw_has(g_fw_port, g_fw_from)) fw_rule("-D", g_fw_port, g_fw_from);
    g_fw_port = 0;
}

static void push_listen(void) {
    time_t now = time(NULL);
    if (g_listen_port == g_conf.push_port && !strcmp(g_listen_from, g_conf.push_from)) {
        static time_t last_fw;                      /* re-check: a firewall reload drops our rule */
        if (g_listen >= 0 && now - last_fw >= 300) { last_fw = now; fw_open(g_listen_port, g_listen_from); }
        if (g_listen >= 0 || !g_listen_port || now - g_listen_try < 30) return;   /* retry a failed bind */
    } else {
        if (g_listen >= 0) { close(g_listen); g_listen = -1; }
        fw_close();
        g_listen_port = g_conf.push_port;
        snprintf(g_listen_from, sizeof g_listen_from, "%s", g_conf.push_from);
        if (!g_conf.push_port) return;
    }
    g_listen_try = now;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_conf.push_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) {
        ftlog("push: cannot listen on port %d: %s", g_conf.push_port, strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_listen = fd;
    ftlog("push: listening on port %d", g_conf.push_port);
    fw_open(g_conf.push_port, g_conf.push_from);
}

static void http_reply(int fd, int code, const char * json) {
    char hdr[256];
    const char * msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                     : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : "Error";
    int n = snprintf(hdr, sizeof hdr, "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", code, msg, (int)strlen(json));
    ft_send_all(fd, hdr, (size_t)n);
    ft_send_all(fd, json, strlen(json));
}

/* recv with one deadline for the whole request, so a slow client can't stall the daemon */
static ssize_t recv_until(int fd, void * buf, size_t n, time_t deadline) {
    time_t left = deadline - time(NULL);
    if (left <= 0) return -1;
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, (int)left * 1000) <= 0) return -1;
    return recv(fd, buf, n, MSG_DONTWAIT);
}

static void push_handle(int fd) {
    time_t deadline = time(NULL) + 5;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char head[4096];
    size_t hl = 0;
    char * end = NULL;
    while (hl < sizeof head - 1) {
        ssize_t n = recv_until(fd, head + hl, sizeof head - 1 - hl, deadline);
        if (n <= 0) return;
        hl += (size_t)n;
        head[hl] = 0;
        if ((end = strstr(head, "\r\n\r\n"))) break;
    }
    if (!end) { http_reply(fd, 400, "{\"ok\":false}"); return; }
    *end = 0;
    char * body_start = end + 4;
    size_t have = hl - (size_t)(body_start - head);

    char method[8] = "", path[64] = "";
    sscanf(head, "%7s %63s", method, path);
    long clen = -1;
    char token[80] = "";
    for (char * line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        char * l = line + 2;
        if (!strncasecmp(l, "Content-Length:", 15)) clen = atol(l + 15);
        else if (!strncasecmp(l, "X-Toon-Token:", 13)) {
            char * v = l + 13;
            while (*v == ' ') v++;
            size_t vl = strcspn(v, "\r\n");
            if (vl >= sizeof token) vl = sizeof token - 1;
            memcpy(token, v, vl);
            token[vl] = 0;
        }
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/")) { http_reply(fd, 200, "{\"app\":\"bambuddy\",\"ok\":true}"); return; }
    if (strcmp(method, "POST") != 0) { http_reply(fd, 404, "{\"ok\":false}"); return; }
    /* constant-time compare of the shared secret */
    size_t tl = strlen(g_conf.push_token), gl = strlen(token);
    unsigned diff = (unsigned)(tl ^ gl) | (tl == 0);
    for (size_t i = 0; i < tl; i++) diff |= (unsigned char)g_conf.push_token[i] ^ (unsigned char)(i < gl ? token[i] : 0);
    if (diff) { http_reply(fd, 401, "{\"ok\":false}"); return; }
    long max = !strcmp(path, "/cover") ? MAX_COVER : MAX_STATUS;
    if (clen < 0 || clen > max) { http_reply(fd, 413, "{\"ok\":false}"); return; }

    char * body = (char *)malloc((size_t)clen + 1);
    if (!body) return;
    if (have > (size_t)clen) have = (size_t)clen;
    memcpy(body, body_start, have);
    while ((long)have < clen) {
        ssize_t n = recv_until(fd, body + have, (size_t)clen - have, deadline);
        if (n <= 0) { free(body); return; }
        have += (size_t)n;
    }
    body[clen] = 0;

    if (!strcmp(path, "/status")) {
        if (!accept_status(body, "push")) { http_reply(fd, 400, "{\"ok\":false,\"error\":\"not a status\"}"); free(body); return; }
        char file[FILE_N];
        current_file(file, sizeof file);
        int need = file[0] && strcmp(file, g_cover_for) != 0;
        http_reply(fd, 200, need ? "{\"ok\":true,\"need_cover\":true}" : "{\"ok\":true,\"need_cover\":false}");
        publish();
    } else if (!strcmp(path, "/cover")) {
        if (!png_ok((const unsigned char *)body, (size_t)clen)) {
            http_reply(fd, 400, "{\"ok\":false,\"error\":\"not a PNG up to 1024x1024\"}");
            free(body);
            return;
        }
        if (ft_write_atomic(COVER_PATH, body, (size_t)clen, 0) == 0) {
            current_file(g_cover_for, sizeof g_cover_for);
            ftlog("push: cover for %s (%ld bytes)", g_cover_for, clen);
        }
        http_reply(fd, 200, "{\"ok\":true}");
        publish();
    } else {
        http_reply(fd, 404, "{\"ok\":false}");
    }
    free(body);
}

static void push_accept(void) {
    if (g_listen < 0) return;
    for (int i = 0; i < 4; i++) {                  /* a few per pass, then back to the loop */
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(g_listen, (struct sockaddr *)&peer, &pl);
        if (fd < 0) return;
        struct in_addr want;
        if (g_conf.push_from[0] && inet_pton(AF_INET, g_conf.push_from, &want) == 1 &&
            peer.sin_addr.s_addr != want.s_addr && peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        push_handle(fd);
        close(fd);
    }
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char ** argv) {
    int once = argc > 1 && !strcmp(argv[1], "--once");
    ftapp_init();
    load_conf();
    if (once) { pull_once(); publish(); return g_status_json ? 0 : 1; }

    time_t last_pull = 0, last_pub = 0;
    while (ftapp_running) {
        load_conf();
        push_listen();
        ftapp_bxt_service();
        time_t now = time(NULL);
        if (g_conf.url[0] && now - last_pull >= g_conf.poll_s) {
            last_pull = now;
            pull_once();
            publish();
            last_pub = now;
        }
        char * cmd;
        while ((cmd = ftapp_next_cmd())) {           /* {"cmd":"refresh"} from the UI */
            free(cmd);
            last_pull = 0;
            g_printer_resolved = 0;
        }
        if (now - last_pub >= 10) { publish(); last_pub = now; }
        struct pollfd pf = { g_listen, POLLIN, 0 };
        if (g_listen >= 0) { if (poll(&pf, 1, 500) > 0) push_accept(); }
        else usleep(500 * 1000);
    }
    fw_close();
    ftlog("stopping");
    return 0;
}
