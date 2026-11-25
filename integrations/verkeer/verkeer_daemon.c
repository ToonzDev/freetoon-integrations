/*
 * verkeer_daemon.c - FreeToon "Verkeer" app: current traffic jams and road
 * closures in the Netherlands from NDW open data (CC0 1.0).
 *
 *   https://opendata.ndw.nu/actueel_beeld.xml.gz   (DATEX II, refreshed every minute)
 *
 * The feed has no road numbers or place names: those come from the VILD
 * location table (alert-C codes), shipped as vild.tsv (tools/make_vild.py).
 *
 * Every INTERVAL minutes (verkeer.conf, default 5):
 *   - download the feed with curl (only when changed), unpack with gunzip,
 *   - keep jams (AbnormalTraffic) and closures (road / carriageway closed),
 *   - write items.tsv for the detail screen and state.json,
 *   - POST the home tile text to FreeToon (127.0.0.1:10081).
 *
 * verkeer.conf:  WEGEN=A2,A12   roads to watch (empty: national total only)
 *                INTERVAL=5     minutes (3..30)
 * Tests: VK_DIR (base dir), VK_URL (feed URL or local .gz), VK_CORE
 * (host:port of FreeToon, "-" = don't post), VK_ONCE=1 (one round).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define FEED_URL   "https://opendata.ndw.nu/actueel_beeld.xml.gz"
#define MAX_FEED   (24 * 1024 * 1024)   /* unpacked; ~4 MB today */
#define MAX_ITEMS  1500
#define MAX_ROADS  12

static char g_dir[200] = "/mnt/data/integrations/verkeer";
static char g_url[300] = FEED_URL;
static char g_core[64] = "127.0.0.1:10081";
static char g_tmp[200] = "/var/volatile/tmp/verkeer";

static void logf_(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char * fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "[%s] [verkeer] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- VILD: alert-C location -> road + name ------------------------------ */

typedef struct { int loc; char road[8]; char name[48]; } vild_t;
static vild_t * g_vild;
static int      g_nvild;

static int vild_cmp(const void * a, const void * b) {
    return ((const vild_t *)a)->loc - ((const vild_t *)b)->loc;
}

static void vild_load(void) {
    char path[260];
    snprintf(path, sizeof path, "%s/vild.tsv", g_dir);
    FILE * f = fopen(path, "r");
    if (!f) { logf_("no %s: roads unknown", path); return; }
    int cap = 12000;
    g_vild = calloc((size_t)cap, sizeof *g_vild);
    char line[200];
    while (g_vild && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char * a = strtok(line, "\t\r\n"), * b = strtok(NULL, "\t\r\n"), * c = strtok(NULL, "\t\r\n");
        if (!a || !b || !c) continue;
        if (g_nvild == cap) {
            vild_t * n = realloc(g_vild, (size_t)(cap *= 2) * sizeof *g_vild);
            if (!n) break;
            g_vild = n;
        }
        vild_t * v = &g_vild[g_nvild++];
        v->loc = atoi(a);
        snprintf(v->road, sizeof v->road, "%s", b);
        snprintf(v->name, sizeof v->name, "%s", c);
    }
    fclose(f);
    if (g_vild) qsort(g_vild, (size_t)g_nvild, sizeof *g_vild, vild_cmp);
    logf_("%d VILD locations", g_nvild);
}

static const vild_t * vild_find(int loc) {
    if (!g_vild || loc <= 0) return NULL;
    vild_t key = { .loc = loc };
    return bsearch(&key, g_vild, (size_t)g_nvild, sizeof *g_vild, vild_cmp);
}

/* ---- config --------------------------------------------------------------- */

static char g_roads[MAX_ROADS][8];
static int  g_nroads;
static int  g_interval = 5;

static void conf_load(void) {
    char path[260], line[256];
    snprintf(path, sizeof path, "%s/verkeer.conf", g_dir);
    g_nroads = 0;
    g_interval = 5;
    FILE * f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "WEGEN=", 6)) {
            /* "A2, a12 N201" -> A2 A12 N201 */
            for (char * t = strtok(line + 6, " ,;"); t && g_nroads < MAX_ROADS; t = strtok(NULL, " ,;")) {
                char r[8];
                size_t k = 0;
                for (; t[k] && k < sizeof r - 1; k++) r[k] = (char)toupper((unsigned char)t[k]);
                r[k] = 0;
                if ((r[0] == 'A' || r[0] == 'N' || r[0] == 'S') && isdigit((unsigned char)r[1]))
                    snprintf(g_roads[g_nroads++], sizeof g_roads[0], "%s", r);
            }
        } else if (!strncmp(line, "INTERVAL=", 9)) {
            int v = atoi(line + 9);
            if (v >= 3 && v <= 30) g_interval = v;
        }
    }
    fclose(f);
}

static int is_mine(const char * road) {
    for (int i = 0; i < g_nroads; i++)
        if (!strcmp(g_roads[i], road)) return 1;
    return 0;
}

/* ---- small XML helpers (the feed is machine-written, one namespace style) */

/* Text of the first <tag>...</tag> in [p, end); "" when absent. */
static void tag_text(const char * p, const char * end, const char * tag, char * out, size_t n) {
    char open[64];
    snprintf(open, sizeof open, "<%s>", tag);
    out[0] = 0;
    const char * a = memmem(p, (size_t)(end - p), open, strlen(open));
    if (!a) return;
    a += strlen(open);
    const char * b = memchr(a, '<', (size_t)(end - a));
    if (!b) return;
    size_t l = (size_t)(b - a);
    if (l >= n) l = n - 1;
    memcpy(out, a, l);
    out[l] = 0;
}

/* Entities and UTF-8 folded to plain ASCII (the Toon's UI fonts). */
static void clean_text(char * s) {
    char * o = s;
    for (const char * p = s; *p; ) {
        if (*p == '&') {
            static const struct { const char * e; char c; } E[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
            int hit = 0;
            for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
                if (!strncmp(p, E[i].e, strlen(E[i].e))) { *o++ = E[i].c; p += strlen(E[i].e); hit = 1; break; }
            if (!hit) *o++ = *p++;
        } else if ((unsigned char)*p == 0xC3 && p[1]) {     /* Latin-1 letters: drop the accent */
            static const char * map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
            unsigned c = (unsigned char)p[1];
            *o++ = (c >= 0x80 && c <= 0xBF) ? map[c - 0x80] : '?';
            p += 2;
        } else if ((unsigned char)*p >= 0x80) {
            p++;                                            /* other multibyte: skipped */
        } else {
            *o++ = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
            p++;
        }
    }
    *o = 0;
}

static time_t parse_iso(const char * s) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm);                                    /* the feed is in UTC (Z) */
}

static double hav_km(double la1, double lo1, double la2, double lo2) {
    double r = M_PI / 180, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * r) * cos(la2 * r) * sin(dlo / 2) * sin(dlo / 2);
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

/* Length of the first line string in the record (km). */
static double line_km(const char * p, const char * end) {
    const char * a = memmem(p, (size_t)(end - p), "<loc:posList>", 13);
    if (!a) return 0;
    a += 13;
    const char * b = memchr(a, '<', (size_t)(end - a));
    if (!b) return 0;
    double km = 0, pla = 0, plo = 0;
    int have = 0;
    char * q = (char *)a;
    while (q < b) {
        char * e1, * e2;
        double la = strtod(q, &e1);
        if (e1 == q || e1 >= b) break;
        double lo = strtod(e1, &e2);
        if (e2 == e1 || e2 > b) break;
        if (have) km += hav_km(pla, plo, la, lo);
        pla = la; plo = lo; have = 1;
        q = e2;
    }
    return km;
}

/* alert-C location after `marker` (e.g. "PrimaryPointLocation"). */
static int alertc_loc(const char * p, const char * end, const char * marker) {
    const char * a = memmem(p, (size_t)(end - p), marker, strlen(marker));
    if (!a) return 0;
    const char * s = memmem(a, (size_t)(end - a), "<loc:specificLocation>", 22);
    if (!s) return 0;
    return atoi(s + 22);
}

/* ---- items ---------------------------------------------------------------- */

typedef struct {
    char   kind;           /* 'F' jam, 'A' closure */
    int    mine;
    char   road[8];
    double km;
    int    delay_s;
    char   from[48], to[48];
    char   cause[96];
    char   comment[160];
    time_t until;
} item_t;

static item_t g_items[MAX_ITEMS];
static int    g_nitems;

static void parse_record(const char * p, const char * end, time_t now) {
    char type[64] = "";
    const char * t = memmem(p, (size_t)(end - p), "xsi:type=\"sit:", 14);
    if (!t || t > p + 200) return;                    /* the record's own type, at its start */
    t += 14;
    size_t tl = strcspn(t, "\"");
    if (tl >= sizeof type) return;
    memcpy(type, t, tl);
    type[tl] = 0;

    char kind = 0, buf[96];
    if (!strcmp(type, "AbnormalTraffic")) kind = 'F';
    else if (!strcmp(type, "RoadOrCarriagewayOrLaneManagement")) {
        tag_text(p, end, "sit:roadOrCarriagewayOrLaneManagementType", buf, sizeof buf);
        if (!strcmp(buf, "roadClosed") || !strcmp(buf, "carriagewayClosures")) kind = 'A';
    }
    if (!kind) return;

    /* now valid? */
    tag_text(p, end, "com:overallStartTime", buf, sizeof buf);
    time_t st = parse_iso(buf);
    if (st && st > now + 60) return;                   /* planned, not yet */
    tag_text(p, end, "com:overallEndTime", buf, sizeof buf);
    time_t en = parse_iso(buf);
    if (en && en < now) return;
    tag_text(p, end, "sit:operatorActionStatus", buf, sizeof buf);
    if (buf[0] && strcmp(buf, "implemented")) return;

    if (g_nitems >= MAX_ITEMS) return;
    item_t * it = &g_items[g_nitems];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->until = en;

    const vild_t * pr = vild_find(alertc_loc(p, end, "PrimaryPointLocation"));
    const vild_t * se = vild_find(alertc_loc(p, end, "SecondaryPointLocation"));
    const vild_t * any = pr ? pr : se;
    if (!any) return;                                  /* no road (city streets): not shown */
    snprintf(it->road, sizeof it->road, "%s", any->road);
    /* alert-C: the secondary location is the start (tail of a jam), the primary the head */
    if (se) snprintf(it->from, sizeof it->from, "%s", se->name);
    if (pr) snprintf(it->to, sizeof it->to, "%s", pr->name);
    if (!strcmp(it->from, it->to)) it->from[0] = 0;

    tag_text(p, end, "sit:queueLength", buf, sizeof buf);
    it->km = buf[0] ? atof(buf) / 1000.0 : line_km(p, end);
    tag_text(p, end, "sit:delayTimeValue", buf, sizeof buf);
    it->delay_s = (int)atof(buf);
    const char * cd = memmem(p, (size_t)(end - p), "<sit:causeDescription>", 22);
    if (cd) tag_text(cd, end, "com:value lang=\"nl\"", it->cause, sizeof it->cause);
    const char * gc = memmem(p, (size_t)(end - p), "<sit:generalPublicComment>", 26);
    if (gc) tag_text(gc, end, "com:value lang=\"nl\"", it->comment, sizeof it->comment);
    clean_text(it->cause);
    clean_text(it->comment);
    it->mine = is_mine(it->road);
    g_nitems++;
}

static void parse_feed(const char * s, size_t n, time_t now) {
    g_nitems = 0;
    const char * end = s + n;
    const char * p = s;
    for (;;) {
        const char * a = memmem(p, (size_t)(end - p), "<sit:situationRecord ", 21);
        if (!a) break;
        const char * b = memmem(a, (size_t)(end - a), "</sit:situationRecord>", 22);
        if (!b) break;
        parse_record(a, b, now);
        p = b + 22;
    }
}

/* ---- fetch ---------------------------------------------------------------- */

/* Download (only when changed) and unpack into a malloc'ed buffer. */
static char * fetch_feed(size_t * len, int * changed) {
    char gz[320], cmd[2048];
    snprintf(gz, sizeof gz, "%s/actueel_beeld.xml.gz", g_tmp);
    *changed = 1;
    if (strncmp(g_url, "http", 4)) {
        snprintf(gz, sizeof gz, "%s", g_url);          /* tests: a local file */
    } else {
        struct stat st0, st1;
        int had = stat(gz, &st0) == 0;
        snprintf(cmd, sizeof cmd,
                 "curl -s -f -L --connect-timeout 10 --max-time 90 %s%s%s -o '%s.part' '%s'; r=$?; "
                 "[ $r = 0 ] && [ -s '%s.part' ] && mv -f '%s.part' '%s'; rm -f '%s.part'; exit $r",
                 had ? "-z '" : "", had ? gz : "", had ? "'" : "", gz, g_url, gz, gz, gz, gz);
        int rc = system(cmd);
        if (rc != 0) { logf_("download failed (rc %d)", rc); *changed = 0; return NULL; }
        if (had && stat(gz, &st1) == 0 && st1.st_mtime == st0.st_mtime && st1.st_size == st0.st_size) {
            *changed = 0;                              /* 304: same file */
        }
    }
    snprintf(cmd, sizeof cmd, "gunzip -c '%s'", gz);
    FILE * f = popen(cmd, "r");
    if (!f) return NULL;
    size_t cap = 6 * 1024 * 1024, n = 0;
    char * buf = malloc(cap + 1);
    while (buf) {
        if (n == cap) {
            if (cap >= MAX_FEED) break;
            char * nb = realloc(buf, (cap *= 2) + 1);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    pclose(f);
    if (!buf || n < 100) { free(buf); logf_("feed empty or unreadable"); return NULL; }
    buf[n] = 0;
    *len = n;
    return buf;
}

/* ---- output ----------------------------------------------------------------- */

static void json_str(FILE * f, const char * s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

static void json_esc(const char * s, char * out, size_t n) {
    size_t o = 0;
    for (; *s && o + 2 < n; s++) {
        if ((unsigned char)*s < 0x20) continue;
        if (*s == '"' || *s == '\\') out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = 0;
}

static void fmt_min(int s, char * out, size_t n) {
    if (s < 60) snprintf(out, n, "%s", "");
    else snprintf(out, n, ", +%d min", (s + 30) / 60);
}

static void post_tile(const char * value, const char * sub, const char * alert) {
    if (!strcmp(g_core, "-")) { logf_("tile: %s | %s", value, sub); return; }
    char host[64];
    snprintf(host, sizeof host, "%s", g_core);
    int port = 10081;
    char * c = strchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        /* no fmemopen: its GLIBC_2.22 version is newer than the Toons' glibc */
        char body[512], req[800], ev[200], es[260], ea[40];
        json_esc(value, ev, sizeof ev);
        json_esc(sub, es, sizeof es);
        json_esc(alert, ea, sizeof ea);
        snprintf(body, sizeof body, "{\"title\":\"Verkeer\",\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"%s\"}",
                 ev, es, ea);
        {
            int n = snprintf(req, sizeof req,
                "POST /api/v1/marketplace/apps/verkeer/state HTTP/1.0\r\nHost: %s\r\n"
                "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                g_core, (int)strlen(body), body);
            if (n > 0 && n < (int)sizeof req) {
                if (send(s, req, (size_t)n, 0) == n) { char r[64]; (void)recv(s, r, sizeof r, 0); }
            }
        }
    }
    close(s);
}

static int by_delay(const void * a, const void * b) {
    const item_t * x = a, * y = b;
    if (x->kind != y->kind) return x->kind == 'F' ? -1 : 1;
    if (x->mine != y->mine) return y->mine - x->mine;
    if (x->delay_s != y->delay_s) return y->delay_s - x->delay_s;
    return (y->km > x->km) - (y->km < x->km);
}

static void write_out(time_t now, const char * err) {
    qsort(g_items, (size_t)g_nitems, sizeof *g_items, by_delay);
    int nf = 0, mf = 0, ma = 0;
    double km = 0;
    const item_t * worst_mine = NULL, * longest = NULL;
    for (int i = 0; i < g_nitems; i++) {
        const item_t * it = &g_items[i];
        if (it->kind == 'F') {
            nf++; km += it->km;
            if (!longest || it->km > longest->km) longest = it;
            if (it->mine) { mf++; if (!worst_mine) worst_mine = it; }
        } else if (it->mine) ma++;
    }

    /* items.tsv for the detail screen: header line, then one item per line */
    char path[260], tmp[270];
    snprintf(path, sizeof path, "%s/items.tsv", g_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "#\t%ld\t%d\t%.1f\t", (long)now, nf, km);
        for (int i = 0; i < g_nroads; i++) fprintf(f, "%s%s", i ? "," : "", g_roads[i]);
        fprintf(f, "\t%s\n", err ? err : "");
        for (int i = 0; i < g_nitems; i++) {
            const item_t * it = &g_items[i];
            if (it->kind == 'A' && !it->mine) continue;   /* closures: only on your roads */
            fprintf(f, "%c\t%d\t%s\t%.1f\t%d\t%s\t%s\t%s\t%s\t%ld\n", it->kind, it->mine, it->road,
                    it->km, it->delay_s, it->from, it->to, it->cause, it->comment, (long)it->until);
        }
        fclose(f);
        rename(tmp, path);
    }

    /* home tile */
    char value[96], sub[128], d[24];
    if (err) {
        snprintf(value, sizeof value, "%s", "--");
        snprintf(sub, sizeof sub, "%s", err);
    } else if (g_nroads) {
        if (mf == 0) snprintf(value, sizeof value, "%s", "Geen files");
        else if (mf == 1) {
            fmt_min(worst_mine->delay_s, d, sizeof d);
            snprintf(value, sizeof value, "%s %.0f km%s", worst_mine->road, worst_mine->km < 1 ? 1 : worst_mine->km, d);
        } else snprintf(value, sizeof value, "%d files", mf);
        int o = 0;
        if (ma) o += snprintf(sub + o, sizeof sub - (size_t)o, "%d afsluiting%s | ", ma, ma == 1 ? "" : "en");
        snprintf(sub + o, sizeof sub - (size_t)o, "NL: %d files, %.0f km", nf, km);
    } else {
        snprintf(value, sizeof value, "%d file%s", nf, nf == 1 ? "" : "s");
        if (longest) snprintf(sub, sizeof sub, "%.0f km, langste %s %.0f km", km, longest->road, longest->km);
        else snprintf(sub, sizeof sub, "%s", "Geen files in Nederland");
    }
    post_tile(value, sub, "");       /* no inbox messages: the core turns any alert into one */

    snprintf(path, sizeof path, "%s/state.json", g_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "{\"value\":"); json_str(f, value);
        fprintf(f, ",\"subtitle\":"); json_str(f, sub);
        fprintf(f, ",\"files\":%d,\"km\":%.1f,\"my_files\":%d,\"my_closures\":%d,\"updated\":%ld}\n",
                nf, km, mf, ma, (long)now);
        fclose(f);
        rename(tmp, path);
    }
    logf_("%d files (%.0f km), %d on your roads, %d closures on your roads", nf, km, mf, ma);
}

int main(void) {
    const char * e;
    if ((e = getenv("VK_DIR")) && e[0]) snprintf(g_dir, sizeof g_dir, "%s", e);
    if ((e = getenv("VK_URL")) && e[0]) snprintf(g_url, sizeof g_url, "%s", e);
    if ((e = getenv("VK_CORE")) && e[0]) snprintf(g_core, sizeof g_core, "%s", e);
    if ((e = getenv("VK_TMP")) && e[0]) snprintf(g_tmp, sizeof g_tmp, "%s", e);
    int once = getenv("VK_ONCE") != NULL;
    mkdir(g_tmp, 0755);
    vild_load();
    time_t last_ok = 0;
    for (;;) {
        conf_load();
        size_t n = 0;
        int changed = 0;
        time_t now = time(NULL);
        char * feed = fetch_feed(&n, &changed);
        if (feed) {
            parse_feed(feed, n, now);
            free(feed);
            last_ok = now;
            write_out(now, NULL);
        } else if (!last_ok || now - last_ok > 30 * 60) {
            g_nitems = 0;
            write_out(now, "NDW niet bereikbaar");
        }
        if (once) break;
        /* sleep, but pick up a changed config (Settings page) within a few seconds */
        char cp[260];
        snprintf(cp, sizeof cp, "%s/verkeer.conf", g_dir);
        struct stat cs;
        time_t cm = stat(cp, &cs) == 0 ? cs.st_mtime : 0;
        for (int s = 0; s < g_interval * 60; s += 5) {
            sleep(5);
            if (stat(cp, &cs) == 0 && cs.st_mtime != cm) break;
        }
    }
    return 0;
}
