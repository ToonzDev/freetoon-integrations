/*
 * freetoon-app-inverters - reads a solar inverter on the home network (or
 * its cloud) and hands production to FreeToon (BACKLOG #60, phase 2).
 *
 * The core owns everything solar (source choice, tiles, dim, statistics);
 * this daemon only measures. Every poll it POSTs to the core on
 * 127.0.0.1:10081:
 *   /api/v1/solar/source                   {"app","brand","ok","power_w","today_wh","total_wh","msg"}
 *   /api/v1/marketplace/apps/inverters/state  value/subtitle for the app's own tile
 *
 * Settings: inverters.conf (KEY=VALUE) next to the binary, written by the
 * core's web form from the "settings" block in manifest.json. Re-read when
 * its mtime changes. Secrets never reach a command line or the log: curl
 * gets its arguments from a 0600 config file that is removed right after.
 *
 * Driver logic ported from oepi-loepi's solarpanel-plugins (ENPHASE1,
 * ENPHASE-T1, OMNIK2, SOLISCLOUD1); the WiFi-stick status.html reader and the
 * SunSpec reader (Modbus TCP: SolarEdge, SMA, Fronius, Kostal, ...) are new.
 *
 * Env (tests): INV_DIR = settings dir, INV_CORE = core host:port.
 */
#include "inv_drivers.h"
#include "minijson.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

#define APP_ID      "inverters"
#define DEF_DIR     "/mnt/data/integrations/" APP_ID
#define CONF_NAME   "inverters.conf"
#define MIN_CORE    "1.0.18"      /* first core with /api/v1/solar/source; keep in step with catalog min_toonui */
#define TOKEN_CACHE "envoy_token.cache"
#define RESP_MAX    (256 * 1024)

static char g_dir[256] = DEF_DIR;
static char g_core_host[64] = "127.0.0.1";
static int  g_core_port = 10081;

/* ------------------------------------------------------------ config */
typedef struct {
    char type[24];            /* enphase | solis_local | soliscloud | omnik | sunspec | huawei | sungrow | growatt | solis_modbus */
    char host[128];           /* inverter / logger / Envoy address */
    char envoy_token[2048];   /* firmware 7+: owner token (optional with Enlighten login) */
    char enl_user[128];
    char enl_pass[128];
    char stick_user[64];
    char stick_pass[64];
    char solis_key_id[96];
    char solis_key_secret[96];
    char solis_api[128];
    char mb_port[8];          /* sunspec: Modbus TCP port */
    char mb_unit[8];          /* sunspec: Modbus unit id */
    int  interval;            /* s */
} conf_t;

static conf_t g_conf;
static time_t g_conf_mtime = -1;

static void logf_(const char * fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "%s [inverters] ", ts);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void path_in_dir(char * out, size_t n, const char * name) {
    snprintf(out, n, "%s/%s", g_dir, name);
}

static void set_field(conf_t * c, const char * k, const char * v) {
#define S(key, field) if (!strcmp(k, key)) { snprintf(c->field, sizeof c->field, "%s", v); return; }
    S("TYPE", type) S("HOST", host) S("ENVOY_TOKEN", envoy_token)
    S("ENLIGHTEN_USER", enl_user) S("ENLIGHTEN_PASS", enl_pass)
    S("STICK_USER", stick_user) S("STICK_PASS", stick_pass)
    S("SOLIS_KEY_ID", solis_key_id) S("SOLIS_KEY_SECRET", solis_key_secret)
    S("SOLIS_API", solis_api) S("MB_PORT", mb_port) S("MB_UNIT", mb_unit)
#undef S
    if (!strcmp(k, "INTERVAL")) c->interval = atoi(v);
}

/* 1 = (re)loaded, 0 = unchanged */
static int conf_load(void) {
    char path[300];
    path_in_dir(path, sizeof path, CONF_NAME);
    struct stat st;
    time_t mt = stat(path, &st) == 0 ? st.st_mtime : 0;
    if (mt == g_conf_mtime) return 0;
    g_conf_mtime = mt;
    conf_t c;
    memset(&c, 0, sizeof c);
    snprintf(c.stick_user, sizeof c.stick_user, "admin");
    snprintf(c.stick_pass, sizeof c.stick_pass, "admin");
    snprintf(c.solis_api, sizeof c.solis_api, "https://www.soliscloud.com:13333");
    snprintf(c.mb_port, sizeof c.mb_port, "502");
    snprintf(c.mb_unit, sizeof c.mb_unit, "1");
    c.interval = 30;
    FILE * f = fopen(path, "r");
    if (f) {
        char line[2400];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#' || !strchr(line, '=')) continue;
            char * eq = strchr(line, '=');
            *eq = 0;
            set_field(&c, line, eq + 1);
        }
        fclose(f);
    }
    if (c.interval < 10) c.interval = 10;
    if (c.interval > 600) c.interval = 600;
    g_conf = c;
    return 1;
}

/* Host: letters, digits and . : - _ [ ] only (it goes into a URL). */
static int host_ok(const char * h) {
    if (!*h) return 0;
    for (const char * p = h; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || strchr(".:-_[]", *p)))
            return 0;
    return 1;
}

/* --------------------------------------------------------------- HTTP */
typedef struct {
    int    code;     /* HTTP status, 0 = no connection */
    int    curl;     /* curl's exit code (0 = ok; 7 refused, 28 time-out, 35 TLS, ...) */
    char * body;     /* NUL-terminated, malloc'd (free with resp_free) */
    size_t len;
} resp_t;

static void resp_free(resp_t * r) { free(r->body); r->body = NULL; r->len = 0; }

/* Why a request got no HTTP answer, in words, from curl's exit code. */
static const char * curl_why(int rc) {
    switch (rc) {
    case 6:  return "adres onbekend";
    case 7:  return "verbinding geweigerd";
    case 28: return "geen antwoord (time-out)";
    case 35: return "TLS-fout";
    case 52: return "leeg antwoord";
    case 56: return "verbinding verbroken";
    default: return "geen verbinding";
    }
}

/* Quoted value for a curl config file. */
static void cfg_q(FILE * f, const char * key, const char * v) {
    fprintf(f, "%s = \"", key);
    for (const char * p = v; *p; p++) {
        if (*p == '\r' || *p == '\n') continue;
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputs("\"\n", f);
}

static const char * tmp_dir(void) {
    struct stat st;
    if (stat("/var/volatile/tmp", &st) == 0 && S_ISDIR(st.st_mode)) return "/var/volatile/tmp";
    return "/tmp";
}

/* One HTTP(S) request through curl. headers: NULL-terminated list. data:
 * request body (POST) or NULL (GET). userpass: "user:pass" for basic auth or
 * NULL. insecure: skip TLS verification (only for the Envoy's self-signed
 * certificate on the LAN). Returns 0 when curl ran; r->code says the rest. */
static int http_req(const char * url, const char * const * headers, const char * data,
                    const char * userpass, int insecure, int timeout, resp_t * r) {
    memset(r, 0, sizeof *r);
    char cfg[300];
    snprintf(cfg, sizeof cfg, "%s/inv-curl-XXXXXX", tmp_dir());
    int fd = mkstemp(cfg);
    if (fd < 0) return -1;
    fchmod(fd, 0600);
    FILE * f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(cfg); return -1; }
    cfg_q(f, "url", url);
    fprintf(f, "silent\nmax-time = %d\nconnect-timeout = 8\n", timeout);
    fputs("write-out = \"\\n%{http_code}\"\n", f);   /* curl's config parser turns \n into a newline */
    if (insecure) fputs("insecure\n", f);
    for (int i = 0; headers && headers[i]; i++) cfg_q(f, "header", headers[i]);
    if (userpass) cfg_q(f, "user", userpass);
    if (data) {
        /* data-binary would read a file for a leading '@'; our bodies never start with one */
        cfg_q(f, "data-binary", data[0] == '@' ? "" : data);
    }
    fclose(f);

    char cmd[400];
    snprintf(cmd, sizeof cmd, "curl -K '%s' 2>/dev/null", cfg);
    FILE * p = popen(cmd, "r");
    if (!p) { unlink(cfg); return -1; }
    size_t cap = 8192, n = 0;
    char * buf = malloc(cap);
    if (!buf) { pclose(p); unlink(cfg); return -1; }
    for (;;) {
        if (n + 4096 + 1 > cap) {
            if (cap >= RESP_MAX) {      /* drain and drop the rest */
                char junk[4096];
                while (fread(junk, 1, sizeof junk, p) > 0) {}
                break;
            }
            cap *= 2;
            char * nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        size_t k = fread(buf + n, 1, cap - n - 1, p);
        if (!k) break;
        n += k;
    }
    int st = pclose(p);
    r->curl = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    unlink(cfg);
    buf[n] = 0;
    /* last line = status code from write-out */
    char * nl = strrchr(buf, '\n');
    if (nl) {
        r->code = atoi(nl + 1);
        *nl = 0;
        n = (size_t)(nl - buf);
    } else {
        r->code = atoi(buf);
        buf[0] = 0;
        n = 0;
    }
    r->body = buf;
    r->len = n;
    if (getenv("INV_DEBUG")) logf_("http %s -> %d (%u bytes, curl %d)", url, r->code, (unsigned)n, r->curl);
    return 0;
}

static void url_enc(const char * s, char * out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("-_.~", c))
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

static void json_esc(const char * s, char * out, size_t n) {
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------- the core */
/* Returns the core's HTTP status, 0 when it could not be reached. */
static int core_post(const char * path, const char * body) {
    int status = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_core_port);
    inet_pton(AF_INET, g_core_host, &a.sin_addr);
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        char req[1400];
        int n = snprintf(req, sizeof req,
            "POST %s HTTP/1.0\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            path, g_core_host, g_core_port, (int)strlen(body), body);
        if (n > 0 && n < (int)sizeof req) {
            if (send(s, req, (size_t)n, 0) == n) {
                char resp[256];
                ssize_t r = recv(s, resp, sizeof resp - 1, 0);   /* wait for the answer, then close */
                if (r > 0) {
                    resp[r] = 0;
                    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) status = 0;
                }
            }
        }
    }
    close(s);
    return status;
}

static void fmt_kwh(double wh, char * out, size_t n) {
    long d = (long)(wh / 100.0 + 0.5);           /* 0.1 kWh */
    snprintf(out, n, "%ld,%ld kWh", d / 10, d % 10);
}

static char g_ss_mfr[40];          /* SunSpec manufacturer (common model), once read */

/* Brand for the core's source line: the type, or for SunSpec the
 * manufacturer the inverter reports (letters, digits, space, - . only). */
static const char * brand_name(void) {
    static char b[40];
    const inv_mb_profile_t * mp = inv_mb_profile(g_conf.type);
    if (mp) return mp->brand;
    if (strcmp(g_conf.type, "sunspec") || !g_ss_mfr[0]) return g_conf.type;
    size_t o = 0;
    for (const char * p = g_ss_mfr; *p && o + 1 < sizeof b; p++)
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || strchr(" -.", *p))
            b[o++] = *p;
    b[o] = 0;
    return o ? b : g_conf.type;
}

static void report(int ok, const inv_reading_t * r, const char * msg) {
    char m[200], body[512];
    json_esc(msg ? msg : "", m, sizeof m);
    snprintf(body, sizeof body,
        "{\"app\":\"" APP_ID "\",\"brand\":\"%s\",\"ok\":%d,\"power_w\":%.0f,"
        "\"today_wh\":%.0f,\"total_wh\":%.0f,\"msg\":\"%s\"}",
        brand_name(), ok, ok ? r->power_w : 0.0, ok ? r->today_wh : -1.0, ok ? r->total_wh : -1.0, m);
    int st = core_post("/api/v1/solar/source", body);
    /* A core before 1.0.18 has no solar source endpoint (404, or 401 when its
     * web login is on, as the path is not public there): the readings go
     * nowhere and the /solar settings page does not exist. Say so on the tile;
     * the catalog's min_toonui only stops installs from the panel. */
    static int old_core_logged;
    int old_core = (st == 404 || st == 401);
    if (old_core && !old_core_logged)
        logf_("FreeToon core has no /api/v1/solar/source (HTTP %d): needs %s or newer", st, MIN_CORE);
    old_core_logged = old_core;

    char val[32], sub[80], tile[220];
    if (old_core) {
        snprintf(val, sizeof val, "-");
        snprintf(sub, sizeof sub, "Vereist FreeToon %s", MIN_CORE);
    } else if (ok) {
        snprintf(val, sizeof val, "%.0f W", r->power_w);
        if (r->today_wh >= 0) {
            char k[48];
            fmt_kwh(r->today_wh, k, sizeof k);
            snprintf(sub, sizeof sub, "Vandaag %s", k);
        } else sub[0] = 0;
    } else {
        snprintf(val, sizeof val, "-");
        snprintf(sub, sizeof sub, "%s", g_conf.type[0] ? "Geen verbinding" : "Niet ingesteld");
    }
    snprintf(tile, sizeof tile, "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\"}", val, sub);
    core_post("/api/v1/marketplace/apps/" APP_ID "/state", tile);
}

/* ------------------------------------------------------------ Enphase */
static int  g_envoy_mode;            /* 0 unknown, 1 plain HTTP (firmware < 7), 2 HTTPS + token */
static char g_envoy_tok[2048];       /* token in use (conf or cache) */
static int  g_envoy_tok_from_conf;
static time_t g_envoy_tok_try;       /* last Enlighten token fetch */

/* The cache holds "<host> <token>": a fetched token is only ever sent to the
 * Envoy it was fetched for, never to an address typed in later. */
static void token_cache_load(void) {
    char path[300], line[2300];
    path_in_dir(path, sizeof path, TOKEN_CACHE);
    FILE * f = fopen(path, "r");
    g_envoy_tok[0] = 0;
    if (!f) return;
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char * sp = strchr(line, ' ');
        if (sp) {
            *sp = 0;
            if (!strcmp(line, g_conf.host)) snprintf(g_envoy_tok, sizeof g_envoy_tok, "%s", sp + 1);
        }
    }
    fclose(f);
}

static void token_cache_store(const char * tok) {
    char path[300], tmp[310];
    path_in_dir(path, sizeof path, TOKEN_CACHE);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    FILE * f = fdopen(fd, "w");
    if (!f) { close(fd); return; }
    fprintf(f, "%s %s\n", g_conf.host, tok);
    fclose(f);
    rename(tmp, path);
}

static void token_cache_drop(void) {
    char path[300];
    path_in_dir(path, sizeof path, TOKEN_CACHE);
    unlink(path);
}

/* Owner token through the Enlighten account, as pyenphase / oepi-loepi's
 * ENPHASE-T1 do: login.json -> session_id; entrez /tokens with the Envoy's
 * serial -> JWT (valid about a year). 1 = got one. */
static int envoy_fetch_token(char * msg, size_t msgn) {
    resp_t r;
    char url[256], sn[64] = "";
    snprintf(url, sizeof url, "http://%s/info.xml", g_conf.host);
    if (http_req(url, NULL, NULL, NULL, 0, 10, &r) == 0 && r.code == 200) inv_parse_envoy_serial(r.body, sn, sizeof sn);
    resp_free(&r);
    if (!sn[0]) {
        snprintf(url, sizeof url, "https://%s/info.xml", g_conf.host);
        if (http_req(url, NULL, NULL, NULL, 1, 10, &r) == 0 && r.code == 200) inv_parse_envoy_serial(r.body, sn, sizeof sn);
        resp_free(&r);
    }
    if (!sn[0]) { snprintf(msg, msgn, "Envoy niet gevonden (serienummer onbekend)"); return 0; }

    char eu[400], ep[400], form[900];
    url_enc(g_conf.enl_user, eu, sizeof eu);
    url_enc(g_conf.enl_pass, ep, sizeof ep);
    snprintf(form, sizeof form, "user%%5Bemail%%5D=%s&user%%5Bpassword%%5D=%s", eu, ep);
    const char * fh[] = { "Content-Type: application/x-www-form-urlencoded", NULL };
    if (http_req("https://enlighten.enphaseenergy.com/login/login.json?", fh, form, NULL, 0, 20, &r) != 0) {
        snprintf(msg, msgn, "curl ontbreekt");
        return 0;
    }
    char sid[128] = "";
    if (r.code == 200) mj_str(r.body, "session_id", sid, sizeof sid);
    int code = r.code;
    resp_free(&r);
    if (!sid[0]) {
        snprintf(msg, msgn, code ? "Enlighten-login geweigerd (e-mail of wachtwoord?)" : "Enlighten niet bereikbaar");
        return 0;
    }

    char ju[300], js[300], body[900];
    json_esc(g_conf.enl_user, ju, sizeof ju);
    json_esc(sid, js, sizeof js);
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"serial_num\":\"%s\",\"username\":\"%s\"}", js, sn, ju);
    const char * jh[] = { "Content-Type: application/json", NULL };
    if (http_req("https://entrez.enphaseenergy.com/tokens", jh, body, NULL, 0, 20, &r) != 0) return 0;
    int ok = 0;
    if (r.code == 200 && r.len > 40 && r.len < sizeof g_envoy_tok) {
        char * t = r.body;
        while (*t == ' ' || *t == '\n' || *t == '\r') t++;
        t[strcspn(t, " \r\n")] = 0;
        int jwt = strchr(t, '.') != NULL;
        for (char * c = t; jwt && *c; c++)
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || strchr("-_.", *c)))
                jwt = 0;
        if (jwt) {
            snprintf(g_envoy_tok, sizeof g_envoy_tok, "%s", t);
            token_cache_store(t);
            ok = 1;
        }
    }
    if (!ok) snprintf(msg, msgn, "geen token gekregen van Enphase (HTTP %d)", r.code);
    resp_free(&r);
    if (ok) logf_("Enphase: new owner token fetched for Envoy %s", sn);
    return ok;
}

static int poll_enphase_v1(inv_reading_t * rd, char * msg, size_t msgn) {
    char url[256];
    resp_t r;
    char http_why[64] = "";        /* plain HTTP gave no answer at all: why */
    if (g_envoy_mode != 2) {
        snprintf(url, sizeof url, "http://%s/api/v1/production", g_conf.host);
        if (http_req(url, NULL, NULL, NULL, 0, 10, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
        int ok = r.code == 200 && inv_parse_envoy_v1(r.body, rd);
        int code = r.code, crc = r.curl;
        resp_free(&r);
        if (ok) {
            if (g_envoy_mode != 1) logf_("Enphase: firmware < 7, plain HTTP");
            g_envoy_mode = 1;
            return 1;
        }
        /* No answer on port 80 at all: newer firmware (D8.x) may have plain
         * HTTP closed and only HTTPS open, so try HTTPS + token as well before
         * calling the Envoy unreachable (user report, firmware D8.3.6022). */
        static int http_dead_logged;   /* log once, not every poll */
        if (code == 0) {
            snprintf(http_why, sizeof http_why, "%s", curl_why(crc));
            if (!http_dead_logged) logf_("Enphase: no answer on plain HTTP (curl %d), trying HTTPS + token", crc);
            http_dead_logged = 1;
        } else if (g_envoy_mode != 2)
            logf_("Enphase: HTTP %d on plain HTTP, switching to HTTPS + token (firmware 7+)", code);
        g_envoy_mode = 2;
    }

    if (g_conf.envoy_token[0]) {
        snprintf(g_envoy_tok, sizeof g_envoy_tok, "%s", g_conf.envoy_token);
        g_envoy_tok_from_conf = 1;
    } else {
        g_envoy_tok_from_conf = 0;
        if (!g_envoy_tok[0]) token_cache_load();
        if (!g_envoy_tok[0]) {
            if (!g_conf.enl_user[0] || !g_conf.enl_pass[0]) {
                snprintf(msg, msgn, "Firmware 7+: vul een token of je Enlighten-account in");
                return 0;
            }
            if (g_envoy_tok_try && time(NULL) - g_envoy_tok_try < 900) {
                snprintf(msg, msgn, "wacht op nieuwe token-poging");
                return 0;
            }
            g_envoy_tok_try = time(NULL);
            if (!envoy_fetch_token(msg, msgn)) return 0;
        }
    }

    char auth[2100];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_envoy_tok);
    const char * h[] = { auth, "Accept: application/json", NULL };
    snprintf(url, sizeof url, "https://%s/api/v1/production", g_conf.host);
    if (http_req(url, h, NULL, NULL, 1, 15, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    int ok = r.code == 200 && inv_parse_envoy_v1(r.body, rd);
    int code = r.code, crc = r.curl;
    resp_free(&r);
    if (ok) return 1;
    if (code == 401 || code == 403) {
        if (g_envoy_tok_from_conf) snprintf(msg, msgn, "token geweigerd door de Envoy (verlopen?)");
        else {
            snprintf(msg, msgn, "token verlopen, nieuwe wordt opgehaald");
            g_envoy_tok[0] = 0;
            token_cache_drop();
        }
    } else if (code == 0) {
        if (http_why[0])
            snprintf(msg, msgn, "Envoy niet bereikbaar op %.64s (http: %.30s, https: %s)",
                     g_conf.host, http_why, curl_why(crc));
        else
            snprintf(msg, msgn, "Envoy niet bereikbaar op %.64s via https (%s)", g_conf.host, curl_why(crc));
        g_envoy_mode = 0;          /* retry plain HTTP next time */
    } else snprintf(msg, msgn, "onverwacht antwoord van de Envoy (HTTP %d)", code);
    return 0;
}

/* /production.json next to /api/v1/production. User report (firmware D8,
 * Envoy-S Metered): /api/v1/production froze completely (386 W, lifetime
 * 7969 kWh, today 0) while /production.json kept updating (inverters
 * 889 W / 8104 kWh, production CT 856 W). So when production.json answers:
 *   power    = the production CT ("eim", live) when installed, else the
 *              microinverter reports' wNow from production.json;
 *   lifetime = the reports' whLifetime from production.json (the same kind
 *              of counter as v1's; the core's accumulator re-bases on the
 *              jump from a frozen v1 value instead of counting it);
 *   today    = counted here from that lifetime (v1's froze at 0), from the
 *              first reading of the day (today.state survives restarts).
 * A production.json that is missing is tried again every 30 min. */
static int    g_pj_state = -1;          /* -1 unknown, 0 none / no CT, 1 CT power used */
static time_t g_pj_next;
static long   g_inv_age_logged = -1;

/* Today from a lifetime counter: start of the day kept in today.state. */
static int    g_day_ymd;
static double g_day_start_wh = -1;

static double today_from_lifetime(double wh) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int ymd = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    char path[300];
    path_in_dir(path, sizeof path, "today.state");
    if (g_day_start_wh < 0) {                   /* first call: state from before a restart */
        FILE * f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d %lf", &g_day_ymd, &g_day_start_wh) != 2) g_day_start_wh = -1;
            fclose(f);
        }
    }
    if (g_day_start_wh < 0 || g_day_ymd != ymd || wh < g_day_start_wh) {
        g_day_ymd = ymd;
        g_day_start_wh = wh;
        FILE * f = fopen(path, "w");
        if (f) { fprintf(f, "%d %.0f\n", ymd, wh); fclose(f); }
    }
    return wh - g_day_start_wh;
}

static void enphase_live_power(inv_reading_t * rd) {
    time_t now = time(NULL);
    if (g_pj_state == 0 && now < g_pj_next) return;
    char url[256], auth[2100];
    resp_t r;
    const char * hdr[] = { auth, "Accept: application/json", NULL };
    int https = g_envoy_mode == 2;
    snprintf(url, sizeof url, "%s://%s/production.json?details=1", https ? "https" : "http", g_conf.host);
    if (https) snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_envoy_tok);
    if (http_req(url, https ? hdr : NULL, NULL, NULL, https, 15, &r) != 0) return;
    inv_envoy_prod_t pj;
    int ok = r.code == 200 && inv_parse_envoy_production(r.body, &pj);
    int code = r.code;
    resp_free(&r);
    if (!ok) {
        if (g_pj_state != 0)
            logf_("Enphase: %s, readings from /api/v1/production",
                  code ? "no /production.json" : "/production.json not reachable");
        g_pj_state = 0;
        g_pj_next = now + 1800;
        return;
    }
    if (pj.have_inv && pj.inv_time > 0) {          /* say so when the reports go stale */
        long age = (long)(now - pj.inv_time) / 60;
        if (age >= 30 && (g_inv_age_logged < 0 || age >= g_inv_age_logged + 60)) {
            logf_("Enphase: last microinverter report is %ld min old (Envoy clock), %.0f W", age, pj.inv_w);
            g_inv_age_logged = age;
        }
    }
    double v1_w = rd->power_w, v1_wh = rd->total_wh;
    if (pj.have_eim)      rd->power_w = pj.eim_w;
    else if (pj.have_inv) rd->power_w = pj.inv_w;
    if (rd->power_w < 0) rd->power_w = 0;
    if (pj.have_inv && pj.inv_wh > 0) {
        rd->total_wh = pj.inv_wh;
        rd->today_wh = today_from_lifetime(pj.inv_wh);
    }
    if (g_pj_state != 1)
        logf_("Enphase: readings from /production.json: power %.0f W (%s), lifetime %.0f Wh "
              "(/api/v1/production said %.0f W, %.0f Wh)", rd->power_w,
              pj.have_eim ? "production CT" : "microinverter reports", rd->total_wh, v1_w, v1_wh);
    g_pj_state = 1;
}

static int poll_enphase(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!poll_enphase_v1(rd, msg, msgn)) return 0;
    enphase_live_power(rd);
    return 1;
}

/* --------------------------------------------- WiFi stick (Solis / Omnik) */
static int poll_stick(int omnik, inv_reading_t * rd, char * msg, size_t msgn) {
    const char * pages[2] = { "/status.html", "/js/status.js" };
    if (omnik) { pages[0] = "/js/status.js"; pages[1] = "/status.html"; }
    char up[140];
    snprintf(up, sizeof up, "%s:%s", g_conf.stick_user, g_conf.stick_pass);
    int last = 0;
    for (int i = 0; i < 2; i++) {
        char url[256];
        resp_t r;
        snprintf(url, sizeof url, "http://%s%s", g_conf.host, pages[i]);
        if (http_req(url, NULL, NULL, up, 0, 10, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
        int ok = r.code == 200 &&
                 (strstr(pages[i], ".js") ? inv_parse_stick_js(r.body, rd) : inv_parse_stick_html(r.body, rd));
        last = r.code;
        resp_free(&r);
        if (ok) return 1;
        if (last == 0) break;       /* nothing there: the second page will not answer either */
    }
    if (last == 401) snprintf(msg, msgn, "inlog van de wifi-stick geweigerd");
    else if (last == 0) snprintf(msg, msgn, "stick niet bereikbaar op %s (omvormer slaapt?)", g_conf.host);
    else snprintf(msg, msgn, "geen omvormergegevens op de stick (HTTP %d)", last);
    return 0;
}

/* ---------------------------------------------------------- SolisCloud */
static int poll_soliscloud(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!g_conf.solis_key_id[0] || !g_conf.solis_key_secret[0]) {
        snprintf(msg, msgn, "vul KeyId en KeySecret van SolisCloud in");
        return 0;
    }
    if (strncmp(g_conf.solis_api, "https://", 8) || strpbrk(g_conf.solis_api + 8, "/?#@ ")) {
        snprintf(msg, msgn, "ongeldig API-adres (https://host:poort)");
        return 0;
    }
    static const char * const path = "/v1/api/userStationList";
    static const char * const body = "{\"pageNo\":1,\"pageSize\":10}";
    char date[64], md5[40], auth[200];
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    static const char * const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char * const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(date, sizeof date, "%s, %02d %s %d %02d:%02d:%02d GMT",
             wd[g.tm_wday], g.tm_mday, mo[g.tm_mon], g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
    inv_soliscloud_sign(g_conf.solis_key_id, g_conf.solis_key_secret, body, date, path,
                        md5, sizeof md5, auth, sizeof auth);
    char h_md5[64], h_date[96], h_auth[220], url[200];
    snprintf(h_md5, sizeof h_md5, "Content-MD5: %s", md5);
    snprintf(h_date, sizeof h_date, "Date: %s", date);
    snprintf(h_auth, sizeof h_auth, "Authorization: %s", auth);
    const char * h[] = { "Content-Type: application/json", h_md5, h_date, h_auth, NULL };
    snprintf(url, sizeof url, "%s%s", g_conf.solis_api, path);
    resp_t r;
    if (http_req(url, h, body, NULL, 0, 20, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    int ok = 0;
    if (r.code == 200) {
        char err[120];
        ok = inv_parse_soliscloud(r.body, rd, err, sizeof err);
        if (!ok) snprintf(msg, msgn, "SolisCloud: %s", err);
    } else if (r.code == 0) snprintf(msg, msgn, "SolisCloud niet bereikbaar");
    else if (r.code == 401 || r.code == 403) snprintf(msg, msgn, "SolisCloud weigert de sleutels (HTTP %d)", r.code);
    else snprintf(msg, msgn, "SolisCloud: HTTP %d", r.code);
    resp_free(&r);
    return ok;
}

/* ------------------------------------------------ SunSpec / Modbus TCP */
/* Generic SunSpec reader: find the "SunS" marker (40000, 50000 or 0), walk
 * the model chain to the inverter model (101-103 integer + scale factor,
 * 111-113 float) and read power and lifetime energy from it. A fresh
 * connection per poll, closed right after: SolarEdge serves one Modbus
 * client at a time, and others (a home automation) get their turn. */
static int      g_ss_model = -1;       /* register of the inverter model's ID word */
static int      g_ss_mid, g_ss_len;
static uint16_t g_ss_tid;

static int recv_all(int s, uint8_t * b, int n) {
    int got = 0;
    while (got < n) {
        ssize_t r = recv(s, b + got, (size_t)(n - got), 0);
        if (r <= 0) return -1;
        got += (int)r;
    }
    return got;
}

static int mb_port(void) {
    int p = atoi(g_conf.mb_port);
    return p > 0 && p < 65536 ? p : 502;
}

static int mb_connect(char * msg, size_t n) {
    char port[8], host[128];
    snprintf(port, sizeof port, "%d", mb_port());
    snprintf(host, sizeof host, "%s", g_conf.host);
    if (host[0] == '[') {                       /* [v6] */
        memmove(host, host + 1, strlen(host));
        char * e = strchr(host, ']');
        if (e) *e = 0;
    }
    struct addrinfo hints, * res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(msg, n, "adres '%s' onbekend", g_conf.host);
        return -1;
    }
    int s = socket(res->ai_family, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); snprintf(msg, n, "geen socket"); return -1; }
    struct timeval tv = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        int e = errno;
        if (e == ECONNREFUSED)
            snprintf(msg, n, "verbinding geweigerd op poort %d: staat Modbus TCP aan, en is dit de juiste poort?", mb_port());
        else
            snprintf(msg, n, "geen verbinding met %s:%d (%s)", g_conf.host, mb_port(),
                     e == EAGAIN || e == EINPROGRESS || e == ETIMEDOUT ? "time-out" : strerror(e));
        close(s);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return s;
}

static int mb_read(int s, uint8_t fc, uint16_t addr, uint16_t count, uint16_t * regs, char * msg, size_t n) {
    uint8_t req[12], resp[300];
    int unit = g_conf.mb_unit[0] ? atoi(g_conf.mb_unit) : 1;
    if (unit < 0 || unit > 255) unit = 1;
    uint16_t tid = ++g_ss_tid;
    inv_modbus_req(req, tid, (uint8_t)unit, fc, addr, count);
    if (send(s, req, sizeof req, 0) != (ssize_t)sizeof req) { snprintf(msg, n, "Modbus: versturen mislukt"); return 0; }
    if (recv_all(s, resp, 7) < 0) { snprintf(msg, n, "Modbus: geen antwoord (unit-id %d?)", unit); return 0; }
    int len = (resp[4] << 8) | resp[5];
    if (len < 2 || 6 + len > (int)sizeof resp) { snprintf(msg, n, "Modbus: ongeldig antwoord"); return 0; }
    if (recv_all(s, resp + 7, len - 1) < 0) { snprintf(msg, n, "Modbus: antwoord afgebroken"); return 0; }
    return inv_modbus_resp(resp, 6 + len, tid, fc, count, regs, msg, n);
}

static int ss_discover(int s, char * msg, size_t n) {
    static const uint16_t bases[] = { 40000, 50000, 0 };
    char e[160] = "";
    for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
        uint16_t r[16];
        if (!mb_read(s, 3, bases[b], 2, r, e, sizeof e) || !inv_sunspec_marker(r)) continue;
        int addr = bases[b] + 2;
        for (int step = 0; step < 40 && addr < 65000; step++) {
            if (!mb_read(s, 3, (uint16_t)addr, 2, r, e, sizeof e)) break;
            int id = r[0], len = r[1];
            if (id == 0xffff || len == 0) break;
            if (id == 1 && len >= 16 && mb_read(s, 3, (uint16_t)(addr + 2), 16, r, e, sizeof e))
                inv_sunspec_string(r, 16, g_ss_mfr, sizeof g_ss_mfr);
            if (inv_sunspec_inverter_model(id)) {
                g_ss_model = addr;
                g_ss_mid = id;
                g_ss_len = len > 120 ? 120 : len;
                logf_("SunSpec: %s, inverter model %d at register %d (%d registers)",
                      g_ss_mfr[0] ? g_ss_mfr : "?", id, addr, len);
                return 1;
            }
            addr += 2 + len;
        }
        snprintf(msg, n, "SunSpec gevonden (register %u), maar geen omvormermodel 101-103/111-113", bases[b]);
        return 0;
    }
    if (e[0]) snprintf(msg, n, "geen SunSpec-gegevens (%s)", e);
    else snprintf(msg, n, "geen SunSpec-gegevens op register 40000, 50000 of 0");
    return 0;
}

static int poll_sunspec(inv_reading_t * rd, char * msg, size_t msgn) {
    int s = mb_connect(msg, msgn);
    if (s < 0) return 0;
    int ok = 0;
    if (g_ss_model >= 0 || ss_discover(s, msg, msgn)) {
        uint16_t m[120];
        if (mb_read(s, 3, (uint16_t)(g_ss_model + 2), (uint16_t)g_ss_len, m, msg, msgn)) {
            int st = 0;
            ok = inv_sunspec_parse(g_ss_mid, m, g_ss_len, rd, &st);
            if (!ok) snprintf(msg, msgn, "omvormer geeft (nog) geen vermogen of opbrengst (status %d)", st);
        } else {
            g_ss_model = -1;                  /* find the layout again next time */
        }
    }
    close(s);
    return ok;
}

/* --------------------------------------- brand Modbus maps (non-SunSpec) */
/* Huawei, Sungrow, Growatt, Solis: fixed registers per brand
 * (inv_drivers.c). The raw words are logged once per settings load, so a
 * tester's log shows what the inverter answers. */
static int g_mb_logged;

static int poll_mbprofile(const inv_mb_profile_t * p, inv_reading_t * rd, char * msg, size_t msgn) {
    int s = mb_connect(msg, msgn);
    if (s < 0) return 0;
    if (p->connect_delay_ms) usleep((useconds_t)p->connect_delay_ms * 1000);   /* Huawei: answers only after ~1 s */
    uint16_t pw[2] = { 0 }, tot[2] = { 0 }, day[2] = { 0 };
    int ok = mb_read(s, p->fc, p->power.addr, p->power.kind == MBV_U16 ? 1 : 2, pw, msg, msgn)
          && mb_read(s, p->fc, p->total.addr, p->total.kind == MBV_U16 ? 1 : 2, tot, msg, msgn);
    int have_day = ok && p->today.kind &&
                   mb_read(s, p->fc, p->today.addr, p->today.kind == MBV_U16 ? 1 : 2, day, msg, msgn);
    close(s);
    if (!ok) return 0;
    if (!g_mb_logged) {
        g_mb_logged = 1;
        logf_("%s raw: power[%u] %04x %04x, total[%u] %04x %04x, today[%u] %04x %04x",
              p->brand, p->power.addr, pw[0], pw[1], p->total.addr, tot[0], tot[1], p->today.addr, day[0], day[1]);
    }
    if (!inv_mb_parse(p, pw, tot, have_day ? day : NULL, rd)) {
        snprintf(msg, msgn, "%s: onverwachte waarden (vermogen of totaal ontbreekt of klopt niet)", p->brand);
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- main */
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    const char * d = getenv("INV_DIR");
    if (d && *d) snprintf(g_dir, sizeof g_dir, "%s", d);
    const char * core = getenv("INV_CORE");
    if (core && *core) {
        char h[64];
        int port = 0;
        if (sscanf(core, "%63[^:]:%d", h, &port) == 2 && port > 0) {
            snprintf(g_core_host, sizeof g_core_host, "%s", h);
            g_core_port = port;
        }
    }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);
    logf_("started (settings in %s/%s)", g_dir, CONF_NAME);

    char last_msg[200] = "-";
    int last_ok = -1, zero_polls = 0;
    while (!g_stop) {
        if (conf_load()) {
            logf_("settings loaded: type=%s host=%s", g_conf.type[0] ? g_conf.type : "-", g_conf.host[0] ? g_conf.host : "-");
            g_envoy_mode = 0;
            g_envoy_tok[0] = 0;
            g_envoy_tok_try = 0;
            g_ss_model = -1;
            g_ss_mfr[0] = 0;
            g_mb_logged = 0;
            g_pj_state = -1;
            g_inv_age_logged = -1;
            last_ok = -1;
            zero_polls = 0;
        }
        inv_reading_t rd = { 0, -1, -1 };
        char msg[200] = "";
        int ok = 0, cloud = 0;
        const char * t = g_conf.type;
        const inv_mb_profile_t * mbp = inv_mb_profile(t);
        int local = !strcmp(t, "enphase") || !strcmp(t, "solis_local") || !strcmp(t, "omnik") || !strcmp(t, "sunspec") || mbp;
        if (!t[0]) snprintf(msg, sizeof msg, "geen omvormer ingesteld");
        else if (local && !host_ok(g_conf.host)) snprintf(msg, sizeof msg, "vul het IP-adres in");
        else if (!strcmp(t, "enphase"))     ok = poll_enphase(&rd, msg, sizeof msg);
        else if (!strcmp(t, "solis_local")) ok = poll_stick(0, &rd, msg, sizeof msg);
        else if (!strcmp(t, "omnik"))       ok = poll_stick(1, &rd, msg, sizeof msg);
        else if (!strcmp(t, "sunspec"))     ok = poll_sunspec(&rd, msg, sizeof msg);
        else if (mbp)                       ok = poll_mbprofile(mbp, &rd, msg, sizeof msg);
        else if (!strcmp(t, "soliscloud")) { cloud = 1; ok = poll_soliscloud(&rd, msg, sizeof msg); }
        else snprintf(msg, sizeof msg, "onbekend type '%s'", t);

        report(ok, &rd, ok ? "" : msg);
        if (ok != last_ok || (!ok && strcmp(msg, last_msg))) {
            if (ok) logf_("reading: %.0f W, today %.0f Wh, total %.0f Wh", rd.power_w, rd.today_wh, rd.total_wh);
            else logf_("no reading: %s", msg);
            last_ok = ok;
            snprintf(last_msg, sizeof last_msg, "%s", msg);
        }

        /* Poll at the configured pace while the panels produce; slower at
         * night (0 W or the logger asleep) and for cloud APIs (SolisCloud
         * refreshes every ~5 min and limits request rates). */
        zero_polls = (ok && rd.power_w > 0) ? 0 : zero_polls + 1;
        int wait = g_conf.interval;
        if (cloud && wait < 120) wait = 120;
        if (zero_polls > 10 && wait < 120) wait = 120;
        if (!t[0]) wait = 15;     /* pick up new settings quickly */
        for (int i = 0; i < wait && !g_stop; i++) {
            sleep(1);
            if (i % 5 == 4) {
                char path[300];
                struct stat st;
                path_in_dir(path, sizeof path, CONF_NAME);
                time_t mt = stat(path, &st) == 0 ? st.st_mtime : 0;
                if (mt != g_conf_mtime) break;
            }
        }
    }
    logf_("stopped");
    return 0;
}
