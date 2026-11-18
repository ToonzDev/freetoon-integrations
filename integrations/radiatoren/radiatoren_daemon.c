/*
 * radiatoren_daemon.c - FreeToon integration daemon for the Radiatoren app.
 *
 * Runs from /etc/inittab (respawn, row written by integrations-install.sh)
 * next to toonui. Every second it checks the settings file; whenever it
 * changed, whenever a room's target changes (schedule edge) and at least
 * every 30 s it
 *
 *   1. writes /qmf/www/tsc/radiatoren.json (targets-v1, atomically) - Home
 *      Assistant polls http://<toon>/tsc/radiatoren.json (lighttpd);
 *   2. notifies its BoxTalk service "radiatoren" (127.0.0.1:1337) with the
 *      tile fields <value> and <subtitle>, and posts the same to toonui's
 *      local API (:10081, what the maintained marketplace apps use);
 *   3. writes /mnt/data/integrations/radiatoren/state.json, which toonui
 *      reads at boot to fill the tile before the first notify arrives.
 *
 * The settings are edited by the UI plugin (radiatoren_ui.toon2.so, inside
 * toonui) and live in the QML app's file /mnt/data/tsc/radiatoren.userSettings.json;
 * this daemon only reads them. Logs go to stderr (the installer redirects
 * them to /var/volatile/tmp/integration-radiatoren.log).
 *
 *   radiatoren [--once] [--settings PATH] [--publish PATH]
 */
#define _GNU_SOURCE
#include "radiatoren_logic.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_ID        "radiatoren"
#define BXT_PORT          1337
#define API_PORT          10081
#define PUBLISH_EVERY_S   30
#define ANNOUNCE_EVERY_S  300
#define RECONNECT_EVERY_S 10
#define UUID_PATH         RAD_APP_DIR "/uuid"
#define STATE_PATH        RAD_APP_DIR "/state.json"
#define TOONUI_CFG        "/mnt/data/toonui.cfg"

static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static void logf_(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char * fmt, ...) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s [radiatoren] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- stable UUID ------------------------------------------------------- */

static char g_uuid[64];

static void load_uuid(void) {
    char * text = rad_read_file(UUID_PATH);
    if (text) {
        size_t n = strcspn(text, "\r\n");
        if (n > 8 && n < sizeof g_uuid) { memcpy(g_uuid, text, n); g_uuid[n] = 0; free(text); return; }
        free(text);
    }
    char rnd[40] = "";
    char * k = rad_read_file("/proc/sys/kernel/random/uuid");
    if (k) { snprintf(rnd, sizeof rnd, "%.36s", k); free(k); }
    if (!rnd[0]) snprintf(rnd, sizeof rnd, "%08lx-%04x", (unsigned long)time(NULL), (unsigned)getpid() & 0xffff);
    snprintf(g_uuid, sizeof g_uuid, "radiatoren-%s", rnd);
    rad_mkdirs(RAD_APP_DIR);
    char line[80];
    snprintf(line, sizeof line, "%s\n", g_uuid);
    if (rad_write_atomic(UUID_PATH, line, 1) != 0) logf_("could not store uuid in %s", UUID_PATH);
}

/* ---- BoxTalk ----------------------------------------------------------- */

static int g_bxt = -1;
static time_t g_bxt_last_try, g_bxt_last_announce;
static int g_bxt_fresh;                /* just (re)connected: notify right away */

static int send_all(int fd, const char * data, size_t len) {
    while (len > 0) {
        ssize_t w = send(fd, data, len, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        data += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Every BoxTalk frame is XML followed by one NUL byte. */
static int bxt_frame(const char * xml) {
    if (g_bxt < 0) return -1;
    if (send_all(g_bxt, xml, strlen(xml) + 1) != 0) {
        logf_("boxtalk send failed (%s), reconnecting", strerror(errno));
        close(g_bxt);
        g_bxt = -1;
        return -1;
    }
    return 0;
}

static void bxt_announce(void) {
    char xml[512];
    snprintf(xml, sizeof xml,
             "<discovery nts=\"ssdp:alive\" uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:thirdParty\" "
             "version=\"v\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
             "<service type=\"%s\" version=\"1\"/></discovery>", g_uuid, SERVICE_ID);
    if (bxt_frame(xml) == 0) g_bxt_last_announce = time(NULL);
}

static void bxt_connect_if_needed(time_t now) {
    if (g_bxt >= 0 || now - g_bxt_last_try < RECONNECT_EVERY_S) return;
    g_bxt_last_try = now;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(BXT_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return; }
    g_bxt = fd;
    logf_("connected to boxtalk on 127.0.0.1:%d", BXT_PORT);
    bxt_announce();
    g_bxt_fresh = 1;
}

/* We never need what the hub sends us, but read it so its buffer never fills. */
static void bxt_drain(void) {
    if (g_bxt < 0) return;
    char buf[2048];
    for (;;) {
        ssize_t n = recv(g_bxt, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            logf_("boxtalk connection closed, reconnecting");
            close(g_bxt);
            g_bxt = -1;
        }
        return;
    }
}

/* ---- toonui local API -------------------------------------------------- */

static void api_post_state(const char * value, const char * subtitle) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(API_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        char body[256], req[512];
        int bn = snprintf(body, sizeof body, "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\"}", value, subtitle);
        int rn = snprintf(req, sizeof req,
                          "POST /api/v1/marketplace/apps/" SERVICE_ID "/state HTTP/1.0\r\n"
                          "Host: 127.0.0.1:%d\r\nContent-Type: application/json\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n%s", API_PORT, bn, body);
        if (rn > 0 && rn < (int)sizeof req) send_all(fd, req, (size_t)rn);
        char sink[256];
        while (recv(fd, sink, sizeof sink, 0) > 0) { }
    }
    close(fd);
}

/* ---- tile text ----------------------------------------------------------- */

static int english(void) {
    char * cfg = rad_read_file(TOONUI_CFG);
    int en = 0;
    if (cfg) {
        const char * p = strstr(cfg, "\nlanguage=");
        if (!p && strncmp(cfg, "language=", 9) == 0) p = cfg - 1;
        if (p) en = atoi(p + 10) == 1;
        free(cfg);
    }
    return en;
}

/* Tile text goes into XML and JSON verbatim (toonui does not unescape):
 * keep it to characters that need no escaping. */
static void tile_safe(char * s) {
    for (char * p = s; *p; p++)
        if (*p == '<' || *p == '>' || *p == '&' || *p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
}

static void tile_text(const rad_settings_t * s, const struct tm * tm, char * value, size_t vn,
                      char * sub, size_t sn) {
    int en = english();
    if (s->n_rooms == 0) {
        snprintf(value, vn, "--");
        snprintf(sub, sn, "%s", en ? "No rooms yet" : "Nog geen kamers");
        return;
    }
    const rad_room_t * first = &s->rooms[0];
    rad_target_t t = rad_target_for(first, tm);
    char temp[16];
    rad_format_temp(t.target, temp, sizeof temp);
    if (en) { char * c = strchr(temp, ','); if (c) *c = '.'; }
    if (t.reason == RAD_REASON_OFF) snprintf(value, vn, "%s", en ? "Off" : "Uit");
    else snprintf(value, vn, "%s\xc2\xb0", temp);

    char name[RAD_NAME_BYTES];
    snprintf(name, sizeof name, "%s", first->name);
    tile_safe(name);
    if (s->n_rooms == 1) {
        const char * what = t.reason == RAD_REASON_COMFORT ? "comfort" : t.reason == RAD_REASON_ECO ? "eco"
                          : t.reason == RAD_REASON_MANUAL ? (en ? "manual" : "handmatig") : (en ? "off" : "uit");
        int next = rad_next_change_today(first, tm);
        if (next >= 0) {
            char hm[8];
            rad_format_minutes(next, hm, sizeof hm);
            snprintf(sub, sn, "%.20s, %s %s %s", name, what, en ? "until" : "tot", hm);
        } else {
            snprintf(sub, sn, "%.20s, %s", name, what);
        }
    } else {
        int comfort = 0;
        for (int i = 0; i < s->n_rooms; i++)
            if (rad_target_for(&s->rooms[i], tm).reason == RAD_REASON_COMFORT) comfort++;
        snprintf(sub, sn, en ? "%.14s, %d/%d on comfort" : "%.14s, %d/%d op comfort", name, comfort, s->n_rooms);
    }
}

/* ---- publishing ---------------------------------------------------------- */

static const char * g_settings_path = RAD_SETTINGS_PATH;
static const char * g_publish_path = RAD_PUBLISH_PATH;

static int publish(const rad_settings_t * s) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    int rc = 0;
    char * json = rad_publish_json(s, now, &tm);
    if (json) {
        char dir[256];
        snprintf(dir, sizeof dir, "%s", g_publish_path);
        char * slash = strrchr(dir, '/');
        if (slash && slash != dir) { *slash = 0; rad_mkdirs(dir); }
        if (rad_write_atomic(g_publish_path, json, 0) != 0) {
            logf_("cannot write %s: %s", g_publish_path, strerror(errno));
            rc = -1;
        }
        free(json);
    }

    char value[32], sub[64];
    tile_text(s, &tm, value, sizeof value, sub, sizeof sub);

    char xml[512];
    snprintf(xml, sizeof xml,
             "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
             "<value>%s</value><subtitle>%s</subtitle><rooms>%d</rooms></notify>",
             g_uuid, SERVICE_ID, value, sub, s->n_rooms);
    bxt_frame(xml);

    char state[256];
    snprintf(state, sizeof state, "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\",\"updated\":%lld}\n",
             value, sub, (long long)now);
    rad_mkdirs(RAD_APP_DIR);
    rad_write_atomic(STATE_PATH, state, 0);

    api_post_state(value, sub);
    return rc;
}

/* Detects a change of target or reason in any room (schedule edges). */
static unsigned long targets_signature(const rad_settings_t * s, const struct tm * tm) {
    unsigned long h = 5381;
    for (int i = 0; i < s->n_rooms; i++) {
        rad_target_t t = rad_target_for(&s->rooms[i], tm);
        h = h * 33 + (unsigned long)(t.target * 2) + 1000UL * (unsigned long)t.reason;
    }
    return h * 33 + (unsigned long)s->n_rooms;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char ** argv) {
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--settings") && i + 1 < argc) g_settings_path = argv[++i];
        else if (!strcmp(argv[i], "--publish") && i + 1 < argc) g_publish_path = argv[++i];
        else { fprintf(stderr, "usage: %s [--once] [--settings PATH] [--publish PATH]\n", argv[0]); return 2; }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    load_uuid();
    logf_("starting, settings %s, publishing %s, uuid %s", g_settings_path, g_publish_path, g_uuid);

    static rad_settings_t settings;
    struct stat last = { 0 };
    int have_stat = 0, loaded = 0;
    time_t last_publish = 0;
    unsigned long last_sig = 0;

    while (g_running) {
        time_t now = time(NULL);
        int dirty = 0;

        /* The UI plugin replaces the file with rename(), so the inode changes
         * on every save - even twice within the same second. */
        struct stat st;
        int exists = stat(g_settings_path, &st) == 0;
        if (!loaded || exists != have_stat
            || (exists && (st.st_ino != last.st_ino || st.st_mtime != last.st_mtime || st.st_size != last.st_size))) {
            rad_load_settings(&settings, g_settings_path);
            logf_("%s settings: %d room(s)", loaded ? "reloaded" : "loaded", settings.n_rooms);
            loaded = 1;
            have_stat = exists;
            if (exists) last = st;
            dirty = 1;
        }

        struct tm tm;
        localtime_r(&now, &tm);
        unsigned long sig = targets_signature(&settings, &tm);
        if (sig != last_sig) { last_sig = sig; dirty = 1; }

        bxt_connect_if_needed(now);
        bxt_drain();
        if (g_bxt >= 0 && now - g_bxt_last_announce >= ANNOUNCE_EVERY_S) bxt_announce();
        if (g_bxt_fresh) { g_bxt_fresh = 0; dirty = 1; }

        if (dirty || now - last_publish >= PUBLISH_EVERY_S || now < last_publish) {
            publish(&settings);
            last_publish = now;
        }
        if (once) break;
        sleep(1);
    }

    if (g_bxt >= 0) close(g_bxt);
    logf_("stopped");
    return 0;
}
