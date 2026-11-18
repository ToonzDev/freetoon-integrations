/*
 * matter_daemon.c - FreeToon integration daemon for the Matter Bridge.
 *
 * Runs from /etc/inittab (respawn row written by integrations-install.sh)
 * next to toonui and does three things:
 *
 *   1. Tile: notifies BoxTalk service "matter" (127.0.0.1:1337) with <value>
 *      and <subtitle>, posts the same to toonui's local API (:10081) and keeps
 *      /mnt/data/integrations/matter/state.json for toonui's boot.
 *
 *   2. Keeps the bridge running. The Matter Bridge ToonStore app starts its
 *      Node service from /etc/rc5.d/S99toon-matter at boot, but nothing
 *      restarts it after a crash (the Toon has little RAM; Node is the OOM
 *      killer's preferred victim by design). When the service has been down
 *      for 45 s this daemon runs `toon-matter-service.sh start`, with a back-
 *      off: three tries two minutes apart, then one every 30 minutes.
 *
 *   3. Runs what the screen asks for (command file MB_COMMAND_PATH):
 *        restart   toon-matter-service.sh stop + start
 *        reset     stop, `toon-matter-service.sh cli --bridge --reset`
 *                  (Matterbridge's own "remove the commissioning information"),
 *                  start again with the same pairing code
 *      Progress and the outcome go to MB_ACTION_PATH for the screen. While an
 *      action runs, (2) stays out of the way.
 *
 * Logs go to stderr (/var/volatile/tmp/integration-matter.log).
 *
 *   matter [--once]
 */
#define _GNU_SOURCE
#include "matter_logic.h"

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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_ID        "matter"
#define BXT_PORT          1337
#define API_PORT          10081
#define PUBLISH_EVERY_S   60
#define ANNOUNCE_EVERY_S  300
#define RECONNECT_EVERY_S 10
#define UUID_PATH         MB_APP_DIR "/uuid"
#define STATE_PATH        MB_APP_DIR "/state.json"
#define BUSY_PATH         MB_RUN_DIR "/busy"
#define TOONUI_CFG        "/mnt/data/toonui.cfg"

#define DOWN_GRACE_S      45      /* service down this long before we start it */
#define RETRY_SHORT_S     120
#define RETRY_LONG_S      1800
#define SHORT_TRIES       3
#define HEALTHY_RESET_S   600     /* running this long clears the try counter */

static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static void logf_(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char * fmt, ...) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s [matter] ", ts);
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
    char * text = mb_read_file(UUID_PATH);
    if (text) {
        size_t n = strcspn(text, "\r\n");
        if (n > 8 && n < sizeof g_uuid) { memcpy(g_uuid, text, n); g_uuid[n] = 0; free(text); return; }
        free(text);
    }
    char rnd[40] = "";
    char * k = mb_read_file("/proc/sys/kernel/random/uuid");
    if (k) { snprintf(rnd, sizeof rnd, "%.36s", k); free(k); }
    if (!rnd[0]) snprintf(rnd, sizeof rnd, "%08lx-%04x", (unsigned long)time(NULL), (unsigned)getpid() & 0xffff);
    snprintf(g_uuid, sizeof g_uuid, "matter-%s", rnd);
    mb_mkdirs(MB_APP_DIR);
    char line[80];
    snprintf(line, sizeof line, "%s\n", g_uuid);
    if (mb_write_atomic(UUID_PATH, line, 1) != 0) logf_("could not store uuid in %s", UUID_PATH);
}

/* ---- BoxTalk ----------------------------------------------------------- */

static int g_bxt = -1;
static time_t g_bxt_last_try, g_bxt_last_announce;
static int g_bxt_fresh;

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
    fcntl(fd, F_SETFD, FD_CLOEXEC);
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
    fcntl(fd, F_SETFD, FD_CLOEXEC);
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

static int english(void) {
    char * cfg = mb_read_file(TOONUI_CFG);
    int en = 0;
    if (cfg) {
        const char * p = strstr(cfg, "\nlanguage=");
        if (!p && strncmp(cfg, "language=", 9) == 0) p = cfg - 1;
        if (p) en = atoi(p + 10) == 1;
        free(cfg);
    }
    return en;
}

/* ---- actions (child processes) -------------------------------------------- */

static mb_action_t g_action = MB_ACT_IDLE;
static pid_t       g_child;
static time_t      g_child_started;
static mb_action_t g_last_action = MB_ACT_IDLE;
static int         g_last_failed;
static time_t      g_last_done;
static char        g_last_message[160];
static int         g_autostart_paused;

static void write_action_state(void) {
    char json[512], msg[200];
    /* message: no quotes or backslashes to escape (we write it ourselves) */
    snprintf(msg, sizeof msg, "%s", g_last_message);
    for (char * c = msg; *c; c++) if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = ' ';
    snprintf(json, sizeof json,
             "{\"daemon_pid\":%d,\"action\":\"%s\",\"action_started\":%lld,\"last_action\":\"%s\","
             "\"last_failed\":%s,\"last_done\":%lld,\"last_message\":\"%s\",\"autostart_paused\":%s}\n",
             (int)getpid(), mb_action_name(g_action), (long long)(g_action ? g_child_started : 0),
             mb_action_name(g_last_action), g_last_failed ? "true" : "false", (long long)g_last_done, msg,
             g_autostart_paused ? "true" : "false");
    if (mb_secure_dir(MB_RUN_DIR) != 0) { logf_("%s is not a safe directory, not writing", MB_RUN_DIR); return; }
    if (mb_write_atomic(MB_ACTION_PATH, json, 0) != 0) logf_("cannot write %s: %s", MB_ACTION_PATH, strerror(errno));
}

static int action_timeout(mb_action_t a) { return a == MB_ACT_RESET ? 420 : 120; }

static int start_action(mb_action_t a, const char * script) {
    /* $1 is the service script. `cli --bridge --reset` runs Matterbridge in the
     * foreground once: it clears the fabrics, sessions and commissioning data
     * of the bridge and exits. The passcode and discriminator are fixed in the
     * service script, so the pairing code stays the same. */
    static const char * const scripts[] = {
        [MB_ACT_START]   = "\"$1\" start",
        [MB_ACT_RESTART] = "\"$1\" stop; \"$1\" start",
        [MB_ACT_RESET]   = "\"$1\" stop || exit 3\n"
                           "\"$1\" cli --bridge --reset; rc=$?\n"
                           "\"$1\" start || exit 4\n"
                           "exit $rc",
    };
    /* The lock outlives this daemon if init respawns it mid-job; the next
     * daemon then leaves the service alone until the job is gone. */
    static const char lock_wrap[] =
        "echo $$ > " BUSY_PATH "; trap 'rm -f " BUSY_PATH "' EXIT\n%s\n";
    char job[512];
    snprintf(job, sizeof job, lock_wrap, a > MB_ACT_IDLE && a <= MB_ACT_RESET ? scripts[a] : "exit 2");
    if (g_action != MB_ACT_IDLE || a == MB_ACT_IDLE || !script || !script[0]) return -1;
    pid_t pid = fork();
    if (pid < 0) { logf_("fork failed: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        /* own session: a timeout kill reaches the whole job, and the Node
         * service started here does not die with this daemon's group */
        setsid();
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); if (devnull > 2) close(devnull); }
        dup2(2, 1);                                   /* script output into our log */
        if (chdir("/") != 0) { }
        execl("/bin/sh", "sh", "-c", job, "sh", script, (char *)NULL);
        _exit(127);
    }
    g_action = a;
    g_child = pid;
    g_child_started = time(NULL);
    logf_("%s: running %s (pid %d)", mb_action_name(a), script, (int)pid);
    write_action_state();
    return 0;
}

static void finish_action(int failed, const char * message) {
    logf_("%s %s%s%s", mb_action_name(g_action), failed ? "failed" : "done", message[0] ? ": " : "", message);
    g_last_action = g_action;
    g_last_failed = failed;
    g_last_done = time(NULL);
    snprintf(g_last_message, sizeof g_last_message, "%s", message);
    g_action = MB_ACT_IDLE;
    g_child = 0;
    write_action_state();
}

static void poll_action(time_t now) {
    if (g_action == MB_ACT_IDLE || g_child <= 0) return;
    int st;
    pid_t r = waitpid(g_child, &st, WNOHANG);
    if (r == g_child) {
        char msg[96] = "";
        int failed = !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 3) snprintf(msg, sizeof msg, "stop failed");
        else if (WIFEXITED(st) && WEXITSTATUS(st) == 4) snprintf(msg, sizeof msg, "start failed");
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) snprintf(msg, sizeof msg, "exit code %d", WEXITSTATUS(st));
        else if (WIFSIGNALED(st)) snprintf(msg, sizeof msg, "killed by signal %d", WTERMSIG(st));
        finish_action(failed, msg);
        return;
    }
    if (r < 0 && errno == ECHILD) { finish_action(1, "lost the child process"); return; }
    if (now - g_child_started > action_timeout(g_action)) {
        logf_("%s: no result after %d s, stopping it", mb_action_name(g_action), action_timeout(g_action));
        kill(-g_child, SIGKILL);
        kill(g_child, SIGKILL);
        waitpid(g_child, &st, 0);
        finish_action(1, "timed out");
    }
}

/* A job of a previous daemon (killed by a reinstall) still running? */
static int foreign_job_running(void) {
    int pid = mb_pid_from_file(BUSY_PATH);
    return pid && pid != (int)g_child && mb_pid_alive(pid);
}

static void read_command(const char * script) {
    /* only from our own root-only folder, never through a symlink */
    if (mb_secure_dir(MB_RUN_DIR) != 0) return;
    char * text = mb_read_file_nofollow(MB_COMMAND_PATH);
    unlink(MB_COMMAND_PATH);                      /* also whatever else sits there */
    if (!text) return;
    text[strcspn(text, "\r\n")] = 0;
    mb_action_t a = mb_action_from_name(text);
    if (a != MB_ACT_RESTART && a != MB_ACT_RESET) logf_("ignoring unknown command '%.32s'", text);
    else if (g_action != MB_ACT_IDLE) logf_("ignoring '%s': %s still running", text, mb_action_name(g_action));
    else if (foreign_job_running()) logf_("ignoring '%s': an earlier job is still running", text);
    else if (!script[0]) {
        logf_("ignoring '%s': toon-matter-service.sh not found", text);
        g_last_action = a;
        g_last_failed = 1;
        g_last_done = time(NULL);
        snprintf(g_last_message, sizeof g_last_message, "toon-matter-service.sh not found");
        write_action_state();
    } else {
        logf_("command from the screen: %s", text);
        g_autostart_paused = 0;               /* the user takes charge: fresh tries */
        start_action(a, script);
    }
    free(text);
}

/* ---- keeping the bridge up -------------------------------------------------- */

static time_t g_down_since, g_next_try, g_up_since;
static int    g_tries;

static void supervise(const mb_snapshot_t * s, time_t now) {
    if (g_action != MB_ACT_IDLE || foreign_job_running()) { g_down_since = 0; return; }
    if (s->service == MB_SVC_RUNNING || s->service == MB_SVC_STARTING) {
        g_down_since = 0;
        if (s->service == MB_SVC_RUNNING) {
            if (!g_up_since) g_up_since = now;
            if (g_tries && now - g_up_since >= HEALTHY_RESET_S) {
                logf_("bridge healthy for %d min, retry counter cleared", HEALTHY_RESET_S / 60);
                g_tries = 0;
                if (g_autostart_paused) { g_autostart_paused = 0; write_action_state(); }
            }
        }
        return;
    }
    g_up_since = 0;
    if (s->service != MB_SVC_STOPPED) return;           /* not installed: nothing to start */
    if (!g_down_since) { g_down_since = now; logf_("bridge is not running"); }
    if (now - g_down_since < DOWN_GRACE_S || now < g_next_try) return;
    g_tries++;
    g_next_try = now + (g_tries < SHORT_TRIES ? RETRY_SHORT_S : RETRY_LONG_S);
    int paused = g_tries >= SHORT_TRIES;
    if (paused != g_autostart_paused) g_autostart_paused = paused;
    logf_("starting the bridge (try %d, next check in %d min)", g_tries, (int)((g_next_try - now) / 60));
    start_action(MB_ACT_START, s->script);
}

/* ---- tile ---------------------------------------------------------------- */

static void tile_text(const mb_snapshot_t * s, int en, char * value, size_t vn, char * sub, size_t sn) {
    char code[24];
    mb_format_code(s->pairing_code, code, sizeof code);
    if (g_action == MB_ACT_RESET) {
        snprintf(value, vn, "...");
        snprintf(sub, sn, "%s", en ? "Removing pairing" : "Koppeling wissen");
    } else if (g_action != MB_ACT_IDLE || s->service == MB_SVC_STARTING) {
        snprintf(value, vn, "...");
        snprintf(sub, sn, "%s", en ? "Bridge is starting" : "Bridge start op");
    } else if (s->service == MB_SVC_MISSING) {
        snprintf(value, vn, "--");
        snprintf(sub, sn, "%s", en ? "Bridge not installed" : "Bridge niet geinstalleerd");
    } else if (s->service == MB_SVC_STOPPED) {
        snprintf(value, vn, "%s", en ? "Off" : "Uit");
        snprintf(sub, sn, "%s", en ? "Service stopped" : "Service gestopt");
    } else if (s->commissioned) {
        snprintf(value, vn, "%s", en ? "On" : "Aan");
        if (en) snprintf(sub, sn, "%d device%s shared", s->n_selected, s->n_selected == 1 ? "" : "s");
        else snprintf(sub, sn, "%d %s gedeeld", s->n_selected, s->n_selected == 1 ? "apparaat" : "apparaten");
    } else {
        snprintf(value, vn, "%s", en ? "Pair" : "Koppel");
        if (code[0]) snprintf(sub, sn, "%s %s", en ? "Code" : "Code", code);
        else snprintf(sub, sn, "%s", en ? "Ready to pair" : "Klaar om te koppelen");
    }
}

/* The tile text goes into a BoxTalk XML frame and a JSON body verbatim
 * (toonui does not unescape), and part of it (the setup code) comes from the
 * bridge's status file: keep only characters that need no escaping. */
static void tile_safe(char * s) {
    for (char * p = s; *p; p++)
        if (*p == '<' || *p == '>' || *p == '&' || *p == '"' || *p == '\'' || *p == '\\'
            || (unsigned char)*p < 0x20)
            *p = ' ';
}

static void publish(char * value, char * sub, int selected) {
    tile_safe(value);
    tile_safe(sub);
    char xml[512];
    snprintf(xml, sizeof xml,
             "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
             "<value>%s</value><subtitle>%s</subtitle><shared>%d</shared></notify>",
             g_uuid, SERVICE_ID, value, sub, selected);
    bxt_frame(xml);

    char state[256];
    snprintf(state, sizeof state, "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\",\"updated\":%lld}\n",
             value, sub, (long long)time(NULL));
    mb_mkdirs(MB_APP_DIR);
    mb_write_atomic(STATE_PATH, state, 0);

    api_post_state(value, sub);
}

/* ---- main ---------------------------------------------------------------- */

static const char * svc_name(mb_service_t s) {
    switch (s) {
    case MB_SVC_RUNNING: return "running";
    case MB_SVC_STARTING: return "starting";
    case MB_SVC_STOPPED: return "stopped";
    default: return "missing";
    }
}

int main(int argc, char ** argv) {
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else { fprintf(stderr, "usage: %s [--once]\n", argv[0]); return 2; }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    load_uuid();
    mb_paths_t paths;
    mb_default_paths(&paths);
    logf_("starting, uuid %s", g_uuid);
    unlink(MB_COMMAND_PATH);          /* a command left from before a reboot is stale */
    write_action_state();

    char last_tile[160] = "";
    time_t last_publish = 0;
    mb_service_t last_service = (mb_service_t)-1;
    int last_commissioned = -1;

    while (g_running) {
        time_t now = time(NULL);
        poll_action(now);

        mb_snapshot_t s;
        int en = english();
        mb_snapshot_load(&s, &paths, en);
        if (s.service != last_service || s.commissioned != last_commissioned) {
            logf_("bridge %s%s, %d of %d device(s) shared", svc_name(s.service),
                  s.service == MB_SVC_RUNNING ? (s.commissioned ? ", paired" : ", not paired") : "",
                  s.n_selected, s.n_devices);
            last_service = s.service;
            last_commissioned = s.commissioned;
        }

        read_command(s.script);
        supervise(&s, now);

        bxt_connect_if_needed(now);
        bxt_drain();
        if (g_bxt >= 0 && now - g_bxt_last_announce >= ANNOUNCE_EVERY_S) bxt_announce();

        char value[32], sub[64], tile[160];
        tile_text(&s, en, value, sizeof value, sub, sizeof sub);
        snprintf(tile, sizeof tile, "%s|%s", value, sub);
        if (g_bxt_fresh || strcmp(tile, last_tile) != 0 || now - last_publish >= PUBLISH_EVERY_S || now < last_publish) {
            g_bxt_fresh = 0;
            publish(value, sub, s.n_selected);
            snprintf(last_tile, sizeof last_tile, "%s", tile);
            last_publish = now;
        }
        if (once) break;
        sleep(1);
    }

    if (g_bxt >= 0) close(g_bxt);
    logf_("stopped");
    return 0;
}
