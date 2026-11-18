/*
 * ftapp.h - daemon runtime for FreeToon integrations (header-only).
 *
 * Define FTAPP_ID ("bambuddy", ...) before including. Gives you:
 *   - logging to stderr (init redirects it to /var/volatile/tmp/integration-<id>.log)
 *   - atomic file writes, small file reads, key=value config files
 *   - a stable BoxTalk uuid kept in FTAPP_DIR/uuid
 *   - ftapp_tile(value, subtitle, alert): BoxTalk notify on service FTAPP_ID
 *     plus a POST to toonui's :10081 marketplace state API (the alert field
 *     raises / clears an Inbox notification)
 *   - ftapp_run(): run a program (curl) without a shell and capture stdout
 *   - command files: the UI plugin drops FTAPP_RUN/cmd-*.json, the daemon
 *     picks them up with ftapp_next_cmd()
 *
 * Live state goes to tmpfs (FTAPP_RUN = /var/volatile/tmp/<id>) so polling
 * never wears the flash; settings live in FTAPP_DIR (/mnt/data/integrations/<id>),
 * which survives package updates.
 *
 * Shared, identical copy in every integration folder that uses it.
 */
#ifndef FTAPP_H
#define FTAPP_H

#ifndef FTAPP_ID
#error "define FTAPP_ID before including ftapp.h"
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FTAPP_DIR
#define FTAPP_DIR "/mnt/data/integrations/" FTAPP_ID
#endif
#ifndef FTAPP_RUN
#define FTAPP_RUN "/var/volatile/tmp/" FTAPP_ID
#endif
#ifndef MSG_NOSIGNAL                 /* host builds (macOS); SIGPIPE is ignored anyway */
#define MSG_NOSIGNAL 0
#endif
#define FTAPP_BXT_PORT 1337
#define FTAPP_API_PORT 10081

static volatile sig_atomic_t ftapp_running = 1;
static void ftapp_on_signal_(int s) { (void)s; ftapp_running = 0; }

static void ftlog(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static void ftlog(const char * fmt, ...) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s [%s] ", ts, FTAPP_ID);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- files ---------------------------------------------------------------- */

static char * ft_read_file(const char * path, size_t * len_out) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char * buf = (char *)malloc(cap);
    while (buf) {
        size_t r = fread(buf + len, 1, cap - len - 1, f);
        len += r;
        if (r == 0) break;
        if (cap - len < 2) {
            if (cap > (8u << 20)) break;
            char * nb = (char *)realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (buf) buf[len] = 0;
    if (len_out) *len_out = len;
    return buf;
}

static void ft_mkdirs(const char * dir) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char * p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* tmp + (fsync) + rename, so readers never see half a file. */
static int ft_write_atomic(const char * path, const void * data, size_t len, int durable) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
    unlink(tmp);                                   /* never write through a planted symlink */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    const char * p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        p += w;
        left -= (size_t)w;
    }
    if (durable) fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Change stamp of a file (0 = missing). Includes size and inode, so two
 * atomic saves within the same second (rename = new inode) are both seen. */
static long ft_mtime(const char * path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_mtime ^ ((long)st.st_ino << 12) ^ ((long)st.st_size << 3) ^ 1;
}

/* key=value config (one per line, '#' comments). Copies the value of the
 * LAST matching line into out; returns 1 if found. */
static int ft_conf_get(const char * text, const char * key, char * out, size_t n) {
    int found = 0;
    size_t kl = strlen(key);
    for (const char * line = text; line && *line; ) {
        const char * eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        if (ll > kl && !strncmp(line, key, kl) && line[kl] == '=') {
            size_t vl = ll - kl - 1;
            while (vl > 0 && (line[kl + 1 + vl - 1] == '\r' || line[kl + 1 + vl - 1] == ' ')) vl--;
            if (vl >= n) vl = n - 1;
            memcpy(out, line + kl + 1, vl);
            out[vl] = 0;
            found = 1;
        }
        line = eol ? eol + 1 : NULL;
    }
    return found;
}

/* 1 when toonui is set to English (toonui.cfg language=1). */
static int ft_english(void) {
    char * cfg = ft_read_file("/mnt/data/toonui.cfg", NULL);
    char v[8] = "0";
    if (cfg) { ft_conf_get(cfg, "language", v, sizeof v); free(cfg); }
    return atoi(v) == 1;
}
#define FT_TR(nl, en) (ft_english() ? (en) : (nl))

/* ---- stable uuid ---------------------------------------------------------- */

static char ftapp_uuid[80];

static void ftapp_load_uuid_(void) {
    char * t = ft_read_file(FTAPP_DIR "/uuid", NULL);
    if (t) {
        size_t n = strcspn(t, "\r\n");
        if (n > 8 && n < sizeof ftapp_uuid) { memcpy(ftapp_uuid, t, n); ftapp_uuid[n] = 0; free(t); return; }
        free(t);
    }
    char rnd[40] = "";
    char * k = ft_read_file("/proc/sys/kernel/random/uuid", NULL);
    if (k) { snprintf(rnd, sizeof rnd, "%.36s", k); free(k); }
    if (!rnd[0]) snprintf(rnd, sizeof rnd, "%08lx-%04x", (unsigned long)time(NULL), (unsigned)getpid() & 0xffff);
    snprintf(ftapp_uuid, sizeof ftapp_uuid, "%s-%s", FTAPP_ID, rnd);
    ft_mkdirs(FTAPP_DIR);
    char line[100];
    int n = snprintf(line, sizeof line, "%s\n", ftapp_uuid);
    if (ft_write_atomic(FTAPP_DIR "/uuid", line, (size_t)n, 1) != 0) ftlog("could not store uuid");
}

/* ---- BoxTalk --------------------------------------------------------------- */

static int ftapp_bxt = -1;
static time_t ftapp_bxt_try, ftapp_bxt_announce;

static int ft_send_all(int fd, const char * d, size_t len) {
    while (len > 0) {
        ssize_t w = send(fd, d, len, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        d += w;
        len -= (size_t)w;
    }
    return 0;
}

static int ftapp_bxt_frame_(const char * xml) {        /* XML + one NUL byte */
    if (ftapp_bxt < 0) return -1;
    if (ft_send_all(ftapp_bxt, xml, strlen(xml) + 1) != 0) {
        ftlog("boxtalk send failed (%s), reconnecting", strerror(errno));
        close(ftapp_bxt);
        ftapp_bxt = -1;
        return -1;
    }
    return 0;
}

static void ftapp_bxt_announce_(void) {
    char xml[512];
    snprintf(xml, sizeof xml,
             "<discovery nts=\"ssdp:alive\" uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:thirdParty\" "
             "version=\"v\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
             "<service type=\"%s\" version=\"1\"/></discovery>", ftapp_uuid, FTAPP_ID);
    if (ftapp_bxt_frame_(xml) == 0) ftapp_bxt_announce = time(NULL);
}

static int ft_connect_local(int port, int timeout_s) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

/* Call often (every loop): reconnects, re-announces, drains what the hub sends. */
static void ftapp_bxt_service(void) {
    time_t now = time(NULL);
    if (ftapp_bxt < 0 && now - ftapp_bxt_try >= 10) {
        ftapp_bxt_try = now;
        int fd = ft_connect_local(FTAPP_BXT_PORT, 2);
        if (fd >= 0) {
            ftapp_bxt = fd;
            ftlog("connected to boxtalk");
            ftapp_bxt_announce_();
        }
    }
    if (ftapp_bxt >= 0 && now - ftapp_bxt_announce >= 300) ftapp_bxt_announce_();
    if (ftapp_bxt >= 0) {
        char buf[2048];
        for (;;) {
            ssize_t n = recv(ftapp_bxt, buf, sizeof buf, MSG_DONTWAIT);
            if (n > 0) continue;
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                ftlog("boxtalk closed, reconnecting");
                close(ftapp_bxt);
                ftapp_bxt = -1;
            }
            break;
        }
    }
}

static void ft_xml_esc(const char * in, char * out, size_t n) {
    size_t o = 0;
    for (; in && *in && o + 7 < n; in++) {
        const char * r = NULL;
        if (*in == '<') r = "&lt;"; else if (*in == '>') r = "&gt;"; else if (*in == '&') r = "&amp;";
        else if (*in == '"') r = "&quot;";
        if (r) { size_t rl = strlen(r); memcpy(out + o, r, rl); o += rl; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static void ft_json_esc(const char * in, char * out, size_t n) {
    size_t o = 0;
    for (; in && *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, n - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void ftapp_api_post_(const char * value, const char * subtitle, const char * alert) {
    int fd = ft_connect_local(FTAPP_API_PORT, 2);
    if (fd < 0) return;
    char v[160], s[160], a[200], body[640], req[900];
    ft_json_esc(value, v, sizeof v);
    ft_json_esc(subtitle, s, sizeof s);
    ft_json_esc(alert, a, sizeof a);
    int bn = snprintf(body, sizeof body, "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"%s\"}", v, s, a);
    int rn = snprintf(req, sizeof req,
                      "POST /api/v1/marketplace/apps/" FTAPP_ID "/state HTTP/1.0\r\n"
                      "Host: 127.0.0.1:%d\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n%s", FTAPP_API_PORT, bn, body);
    if (rn > 0 && rn < (int)sizeof req) ft_send_all(fd, req, (size_t)rn);
    char sink[256];
    while (recv(fd, sink, sizeof sink, 0) > 0) { }
    close(fd);
}

/* Publish the tile. Sends on change, and every 60 s anyway (toonui restarts). */
static void ftapp_tile(const char * value, const char * subtitle, const char * alert) {
    static char last[512];
    static time_t last_t;
    char key[512];
    snprintf(key, sizeof key, "%s|%s|%s", value ? value : "", subtitle ? subtitle : "", alert ? alert : "");
    time_t now = time(NULL);
    int changed = strcmp(key, last) != 0;
    if (!changed && now - last_t < 60) return;
    char ev[160], es[160], ea[200], xml[900];
    ft_xml_esc(value, ev, sizeof ev);
    ft_xml_esc(subtitle, es, sizeof es);
    ft_xml_esc(alert, ea, sizeof ea);
    snprintf(xml, sizeof xml,
             "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
             "<value>%s</value><subtitle>%s</subtitle><alert>%s</alert></notify>",
             ftapp_uuid, FTAPP_ID, ev, es, ea);
    int ok = ftapp_bxt_frame_(xml) == 0;
    ftapp_api_post_(value, subtitle, alert);
    /* state.json: toonui reads it at boot to fill the tile before the first notify. */
    char sv[160], ss[160], st[600];
    ft_json_esc(value, sv, sizeof sv);
    ft_json_esc(subtitle, ss, sizeof ss);
    int n = snprintf(st, sizeof st, "{\"value\":\"%s\",\"subtitle\":\"%s\"}\n", sv, ss);
    /* state.json lives on flash (toonui reads it at boot): at most every 10 min */
    static time_t last_flash;
    if (changed && now - last_flash >= 600) { ft_write_atomic(FTAPP_DIR "/state.json", st, (size_t)n, 0); last_flash = now; }
    if (changed) ftlog("tile: %s | %s%s%s%s", value, subtitle, alert && *alert ? " | alert: " : "",
                       alert ? alert : "", ok ? "" : " (boxtalk down)");
    snprintf(last, sizeof last, "%s", key);
    last_t = now;
}

/* ---- run a program, capture stdout --------------------------------------- */

/* argv[0] is looked up in PATH. Returns the exit status (or -1) and a malloc'd,
 * NUL-terminated stdout in *out (caller frees). No shell is involved, so
 * values from config files can't inject commands. */
static int ftapp_run(char * const argv[], char ** out, size_t * out_len, size_t max_bytes, int timeout_s) {
    int pfd[2];
    *out = NULL;
    if (out_len) *out_len = 0;
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); }
        /* don't hand our sockets (BoxTalk, a listening port) to curl */
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t cap = 16384, len = 0;
    char * buf = (char *)malloc(cap);
    time_t start = time(NULL);
    int killed = 0;
    for (;;) {
        struct pollfd pf = { pfd[0], POLLIN, 0 };
        int r = poll(&pf, 1, 500);
        if (r > 0) {
            if (!buf) { kill(pid, SIGKILL); killed = 1; break; }
            if (cap - len < 4096 && cap < max_bytes + 4096) {
                char * nb = (char *)realloc(buf, cap * 2);
                if (nb) { buf = nb; cap *= 2; }
            }
            ssize_t n = read(pfd[0], buf + len, cap - len - 1);
            if (n <= 0) break;
            len += (size_t)n;
            if (len > max_bytes) { kill(pid, SIGKILL); killed = 1; break; }
        }
        if (time(NULL) - start > timeout_s + 2) { kill(pid, SIGKILL); killed = 1; break; }
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (buf) buf[len] = 0;
    *out = buf;
    if (out_len) *out_len = len;
    if (killed) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Write headers (API keys, tokens) to a private file for `curl -H @file`, so
 * they never show up in the process list. 0 when a file was written. */
static int ft_header_file(const char * const * headers, char * path, size_t n) {
    if (!headers || !headers[0]) return -1;
    static unsigned seq;
    snprintf(path, n, FTAPP_RUN "/.hdr-%d-%u", (int)getpid(), seq++);
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { path[0] = 0; return -1; }
    for (int i = 0; headers[i]; i++) {
        if (write(fd, headers[i], strlen(headers[i])) < 0 || write(fd, "\n", 1) < 0) break;
    }
    close(fd);
    return 0;
}

/* GET url with curl. headers: NULL-terminated list of "Name: value". Returns
 * the HTTP status (0 when unreachable) and the body in *body (caller frees). */
static int ft_http_get(const char * url, const char * const * headers, int timeout_s, char ** body, size_t max_bytes) {
    char * argv[40];
    char tmo[16], code_fmt[] = "\n%{http_code}";
    int a = 0;
    snprintf(tmo, sizeof tmo, "%d", timeout_s);
    argv[a++] = (char *)"curl";
    argv[a++] = (char *)"-s";
    argv[a++] = (char *)"-L";
    argv[a++] = (char *)"-m";
    argv[a++] = tmo;
    argv[a++] = (char *)"-w";
    argv[a++] = code_fmt;
    char hfile[128] = "", harg[140];
    if (ft_header_file(headers, hfile, sizeof hfile) == 0) {
        snprintf(harg, sizeof harg, "@%s", hfile);
        argv[a++] = (char *)"-H";
        argv[a++] = harg;
    }
    argv[a++] = (char *)"--";
    argv[a++] = (char *)url;
    argv[a] = NULL;
    char * out = NULL;
    size_t len = 0;
    ftapp_run(argv, &out, &len, max_bytes, timeout_s);
    if (hfile[0]) unlink(hfile);
    *body = NULL;
    if (!out) return 0;
    char * nl = strrchr(out, '\n');
    int code = 0;
    if (nl) { code = atoi(nl + 1); *nl = 0; }
    *body = out;
    return code;
}

/* curl with a method and a body ("PUT"/"POST"). The body goes through a 0600
 * file as well (it may hold a token), never through the command line.
 * Same return as ft_http_get. */
static int ft_http_body(const char * method, const char * url, const char * const * headers, const char * content_type,
                        const char * data, int timeout_s, char ** body) {
    char * argv[40];
    char tmo[16], code_fmt[] = "\n%{http_code}", ctype[96];
    int a = 0;
    snprintf(tmo, sizeof tmo, "%d", timeout_s);
    snprintf(ctype, sizeof ctype, "Content-Type: %s", content_type);
    argv[a++] = (char *)"curl";
    argv[a++] = (char *)"-s";
    argv[a++] = (char *)"-m";
    argv[a++] = tmo;
    argv[a++] = (char *)"-X";
    argv[a++] = (char *)method;
    argv[a++] = (char *)"-w";
    argv[a++] = code_fmt;
    argv[a++] = (char *)"-H";
    argv[a++] = ctype;
    char hfile[128] = "", harg[140], dfile[128] = "", darg[140];
    if (ft_header_file(headers, hfile, sizeof hfile) == 0) {
        snprintf(harg, sizeof harg, "@%s", hfile);
        argv[a++] = (char *)"-H";
        argv[a++] = harg;
    }
    const char * d = data ? data : "";
    static unsigned dseq;
    snprintf(dfile, sizeof dfile, FTAPP_RUN "/.body-%d-%u", (int)getpid(), dseq++);
    unlink(dfile);
    int fd = open(dfile, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (write(fd, d, strlen(d)) < 0) { /* curl then sends what is there */ }
        close(fd);
        snprintf(darg, sizeof darg, "@%s", dfile);
        argv[a++] = (char *)"--data-binary";
        argv[a++] = darg;
    } else {
        dfile[0] = 0;
        argv[a++] = (char *)"--data-binary";
        argv[a++] = (char *)"";
    }
    argv[a++] = (char *)"--";
    argv[a++] = (char *)url;
    argv[a] = NULL;
    char * out = NULL;
    size_t len = 0;
    ftapp_run(argv, &out, &len, 256 * 1024, timeout_s);
    if (hfile[0]) unlink(hfile);
    if (dfile[0]) unlink(dfile);
    *body = NULL;
    if (!out) return 0;
    char * nl = strrchr(out, '\n');
    int code = 0;
    if (nl) { code = atoi(nl + 1); *nl = 0; }
    *body = out;
    return code;
}

static int ft_http_send(const char * method, const char * url, const char * const * headers,
                        const char * json, int timeout_s, char ** body) {
    return ft_http_body(method, url, headers, "application/json", json ? json : "{}", timeout_s, body);
}

/* ---- commands from the UI plugin ------------------------------------------ */

/* Next pending FTAPP_RUN/cmd-*.json (oldest name first), removed after reading.
 * Returns a malloc'd body or NULL. */
static char * ftapp_next_cmd(void) {
    DIR * d = opendir(FTAPP_RUN);
    if (!d) return NULL;
    char best[256] = "";
    struct dirent * e;
    time_t now = time(NULL);
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (strncmp(e->d_name, "cmd-", 4) || nl < 9 || strcmp(e->d_name + nl - 5, ".json") != 0) continue;
        char path[300];
        snprintf(path, sizeof path, FTAPP_RUN "/%s", e->d_name);
        struct stat st;
        /* a command that waited half a minute (daemon busy, Homey away) is stale: drop it */
        if (stat(path, &st) == 0 && now - st.st_mtime > 30) { unlink(path); continue; }
        if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (!best[0]) return NULL;
    char path[300];
    snprintf(path, sizeof path, FTAPP_RUN "/%s", best);
    char * body = ft_read_file(path, NULL);
    unlink(path);
    return body;
}

/* ---- startup -------------------------------------------------------------- */

static void ftapp_init(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, ftapp_on_signal_);
    signal(SIGINT, ftapp_on_signal_);
    signal(SIGCHLD, SIG_DFL);
    ft_mkdirs(FTAPP_DIR);
    /* FTAPP_RUN sits under a world-writable tmp: make sure it is our own directory */
    struct stat st;
    if (lstat(FTAPP_RUN, &st) == 0 && (!S_ISDIR(st.st_mode) || st.st_uid != getuid())) {
        char aside[200];
        snprintf(aside, sizeof aside, FTAPP_RUN ".bad%ld", (long)time(NULL));
        rename(FTAPP_RUN, aside);
        ftlog("moved a foreign %s aside", FTAPP_RUN);
    }
    ft_mkdirs(FTAPP_RUN);
    /* check again after creating: it must be our own directory, not writable by
     * others, or someone could have swapped it in between */
    if (lstat(FTAPP_RUN, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        ftlog("%s is not our own directory, stopping", FTAPP_RUN);
        exit(1);
    }
    if (st.st_mode & 022) chmod(FTAPP_RUN, 0755);  /* toonui (same uid) writes cmd files here */
    ftapp_load_uuid_();
    ftlog("starting, uuid %s", ftapp_uuid);
}

#endif /* FTAPP_H */
