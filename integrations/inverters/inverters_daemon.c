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
#include "inv_crypto.h"
#include "minijson.h"
#include <arpa/inet.h>
#include <ctype.h>
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
    char se_client_id[128];   /* solaredge_cloud: Site Access app (developer.solaredge.com) */
    char se_client_secret[128];
    char se_redirect[256];    /* the Redirect URI registered with that app */
    char gw_user[128];        /* goodwe_sems: SEMS+ account (e-mail) */
    char gw_pass[128];
    char gw_station[96];      /* installation name; "" = the first one */
    char sma_pass[64];        /* sma_web: the "User" password of the SMA web interface */
    char dz_idx[48];          /* domoticz: device idx, or two as "12+19" */
    char pvo_key[96];         /* pvoutput: API key */
    char pvo_sid[24];         /* pvoutput: system id */
    char solax_token[96];     /* solax_cloud: token id */
    char solax_sn[48];        /* solax_cloud: registration number of the wifi dongle */
    char se_auth[1200];       /* fallback: the address SolarEdge sent the browser to (code=...) */
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
    S("SE_CLIENT_ID", se_client_id) S("SE_CLIENT_SECRET", se_client_secret)
    S("SE_REDIRECT", se_redirect) S("SE_AUTH", se_auth)
    S("GW_USER", gw_user) S("GW_PASS", gw_pass) S("GW_STATION", gw_station)
    S("SMA_PASS", sma_pass) S("DZ_IDX", dz_idx) S("PVO_KEY", pvo_key) S("PVO_SID", pvo_sid)
    S("SOLAX_TOKEN", solax_token) S("SOLAX_SN", solax_sn)
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
    if (!strcmp(g_conf.type, "solaredge_cloud")) return "SolarEdge cloud";
    if (!strcmp(g_conf.type, "goodwe_sems")) return "GoodWe SEMS+";
    static const char * const names[][2] = {
        { "fronius", "Fronius" }, { "kostal_piko", "Kostal Piko" }, { "zeversolar", "ZeverSolar" },
        { "opendtu", "openDTU" }, { "saj_local", "SAJ" }, { "fp4all", "FP4ALL" }, { "sma_web", "SMA" },
        { "domoticz", "Domoticz" }, { "pvoutput", "PVOutput" }, { "solax_cloud", "SolaxCloud" } };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(g_conf.type, names[i][0])) return names[i][1];
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

    /* The last good reading: most inverters (and their Wi-Fi sticks) switch
     * off when the panels stop producing, so "no connection" in the evening
     * is normal. Then the tile says the inverter sleeps, with today's yield. */
    static time_t g_good_t;
    static double g_good_w, g_good_today = -1;
    static int    g_good_yday = -1;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (ok) {
        g_good_t = now;
        g_good_w = r->power_w;
        g_good_today = r->today_wh;
        g_good_yday = lt.tm_yday;
    }

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
    } else if (!g_conf.type[0]) {
        snprintf(val, sizeof val, "-");
        snprintf(sub, sizeof sub, "Niet ingesteld");
    } else if (g_good_t && now - g_good_t < 20 * 3600 &&
               (g_good_w < 150 || lt.tm_hour >= 17 || lt.tm_hour < 8)) {
        snprintf(val, sizeof val, "0 W");
        if (g_good_yday == lt.tm_yday && g_good_today >= 0) {
            char k[48];
            fmt_kwh(g_good_today, k, sizeof k);
            snprintf(sub, sizeof sub, "Slaapt, vandaag %s", k);
        } else {
            snprintf(sub, sizeof sub, "Slaapt (geen zon)");
        }
    } else {
        /* the reason in short: the part in brackets ("time-out", "TLS-fout",
         * ...) when there is one, else the message itself */
        char why[64];
        const char * m0 = msg ? msg : "";
        const char * o = strrchr(m0, '(');
        if (o && strchr(o, ')')) {
            size_t l = (size_t)(strchr(o, ')') - o - 1);
            if (l >= sizeof why) l = sizeof why - 1;
            memcpy(why, o + 1, l);
            why[l] = 0;
        } else {
            snprintf(why, sizeof why, "%.63s", m0[0] ? m0 : "geen verbinding");
        }
        if (why[0] >= 'a' && why[0] <= 'z') why[0] = (char)(why[0] - 'a' + 'A');
        if (strlen(why) > 30) { why[27] = '.'; why[28] = '.'; why[29] = '.'; why[30] = 0; }
        snprintf(val, sizeof val, "-");
        snprintf(sub, sizeof sub, "%s", why);
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

/* 0.2.2 fell back to /api/v1/production for 30 min after one failed
 * production.json read: on the reporter's Envoy that is the frozen 386 W, so
 * the tile jumped back to it after a few minutes (user report, 2026-11-25).
 * Now: once production.json has answered (or v1 is known to be frozen), its
 * failure is reported as an error with the reason and retried every poll;
 * v1's numbers are used only for an Envoy where production.json never
 * worked. "v1 frozen" (its lifetime stands still for 30 min while
 * production.json's rises) is remembered in envoy_v1_frozen. */
static int    g_pj_ever;                /* production.json answered since the settings load */
static int    g_v1_frozen = -1;         /* -1 not read yet, 0 no, 1 yes (file) */
static double g_frz_v1_wh = -1, g_frz_pj_wh = -1;
static time_t g_frz_since;

static int v1_frozen(void) {
    if (g_v1_frozen < 0) {
        char path[300];
        path_in_dir(path, sizeof path, "envoy_v1_frozen");
        g_v1_frozen = access(path, F_OK) == 0;
        if (g_v1_frozen) logf_("Enphase: /api/v1/production is known to be frozen on this Envoy (envoy_v1_frozen)");
    }
    return g_v1_frozen;
}

static void v1_frozen_check(double v1_wh, double pj_wh, time_t now) {
    if (g_v1_frozen == 1 || v1_wh <= 0 || pj_wh <= 0) return;
    if (g_frz_since == 0 || v1_wh != g_frz_v1_wh) {      /* v1 moved: start over */
        g_frz_since = now; g_frz_v1_wh = v1_wh; g_frz_pj_wh = pj_wh;
        return;
    }
    const char * fs = getenv("INV_FROZEN_S");          /* tests */
    long frozen_s = fs && atol(fs) > 0 ? atol(fs) : 1800;
    if (now - g_frz_since >= frozen_s && pj_wh > g_frz_pj_wh) {
        g_v1_frozen = 1;
        char path[300];
        path_in_dir(path, sizeof path, "envoy_v1_frozen");
        FILE * f = fopen(path, "w");
        if (f) { fprintf(f, "v1 lifetime %.0f Wh unchanged for 30 min while production.json rose %.0f -> %.0f Wh\n",
                         v1_wh, g_frz_pj_wh, pj_wh); fclose(f); }
        logf_("Enphase: /api/v1/production lifetime stuck at %.0f Wh for 30 min while /production.json rose to %.0f Wh: "
              "not using /api/v1/production numbers any more", v1_wh, pj_wh);
    }
}

/* 1 = rd filled from production.json, 0 = failed (why in why[]). */
static int enphase_production(inv_reading_t * rd, char * why, size_t whyn) {
    time_t now = time(NULL);
    char url[256], auth[2100];
    resp_t r;
    const char * hdr[] = { auth, "Accept: application/json", NULL };
    int https = g_envoy_mode == 2;
    snprintf(url, sizeof url, "%s://%s/production.json", https ? "https" : "http", g_conf.host);
    if (https) snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_envoy_tok);
    if (http_req(url, https ? hdr : NULL, NULL, NULL, https, 25, &r) != 0) { snprintf(why, whyn, "curl ontbreekt"); return 0; }
    inv_envoy_prod_t pj;
    int ok = r.code == 200 && inv_parse_envoy_production(r.body, &pj);
    int code = r.code, crc = r.curl;
    resp_free(&r);
    if (!ok) {
        if (code == 0) snprintf(why, whyn, "/production.json gaf geen antwoord (%s)", curl_why(crc));
        else if (code == 200) snprintf(why, whyn, "/production.json zonder bruikbare productie");
        else snprintf(why, whyn, "/production.json: HTTP %d", code);
        return 0;
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
        v1_frozen_check(v1_wh, pj.inv_wh, now);
    }
    if (g_pj_state != 1)
        logf_("Enphase: readings from /production.json: power %.0f W (%s), lifetime %.0f Wh "
              "(/api/v1/production said %.0f W, %.0f Wh)", rd->power_w,
              pj.have_eim ? "production CT" : "microinverter reports", rd->total_wh, v1_w, v1_wh);
    g_pj_state = 1;
    g_pj_ever = 1;
    return 1;
}

static int poll_enphase(inv_reading_t * rd, char * msg, size_t msgn) {
    /* v1 first: it finds plain HTTP vs HTTPS and gets / renews the token */
    if (!poll_enphase_v1(rd, msg, msgn)) return 0;
    time_t now = time(NULL);
    int must = g_pj_ever || v1_frozen();          /* v1's numbers are not to be trusted here */
    if (!must && g_pj_state == 0 && now < g_pj_next) return 1;   /* older Envoy: v1 only */
    char why[160] = "";
    if (enphase_production(rd, why, sizeof why)) return 1;
    if (must) {
        if (g_pj_state != 2) logf_("Enphase: %s; no reading this round (not falling back to /api/v1/production)", why);
        g_pj_state = 2;
        snprintf(msg, msgn, "Envoy: %s", why);
        return 0;
    }
    if (g_pj_state != 0) logf_("Enphase: %s, readings from /api/v1/production", why);
    g_pj_state = 0;
    g_pj_next = now + 1800;
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

/* ---------------------------------------------------- SolarEdge cloud */
/* Monitoring API v2 ("SolarEdge ONE for Developers"; v1 with the old site API
 * key was switched off in November 2026). A homeowner makes a "Site Access"
 * app on developer.solaredge.com and gets a client id + secret. Linking is an
 * OAuth authorization-code flow: the /solar page (core 1.0.22+) has a
 * "Koppelen" button to connect.solaredge.com/authorize; after approval
 * SolarEdge sends the browser to the registered Redirect URI with
 * ?code=...&site_id=... . When that is the Toon (/solar/callback) the core
 * drops the query in oauth_callback here; otherwise the user pastes the
 * address into SE_AUTH. The code is traded for an access token (about 2 h)
 * and a refresh token that changes on every refresh (so it is stored each
 * time, in solaredge.state). The grant lasts 24 months.
 *
 * Wire details follow the published open-source clients (Home Assistant
 * solaredge_v2, Domoticz, ecoflow): JSON token requests with client id and
 * secret in the body, Bearer calls, /sites/{id}/power (quarter-hour values,
 * 15-30 min behind) and /sites/{id}/lifetime-energy.
 *
 * Budget: the free tier has ~2000 calls a month (10 a minute). Power every
 * 30 min and the lifetime counter every 2 h, 06:00-22:00 only: ~40 calls a
 * day plus token refreshes, ~1500 a month. In between the last values are
 * reported. "Monthly credit limit reached" (HTTP 429) pauses until the 1st. */
#define SE_API   "https://monitoringapi.solaredge.com/v2"
#define SE_STATE "solaredge.state"

typedef struct {
    char   client[128];          /* tokens belong to this client id */
    char   refresh[2048];
    char   access[4096];
    time_t exp;
    char   site[32];
    char   code_done[512];       /* last authorization code used (single use) */
    time_t pause_until;
    time_t power_t, total_t;     /* last successful reads */
    double power_w, total_wh;
} se_state_t;

static se_state_t g_se;
static int g_se_loaded;

/* INV_SE_API: a test server instead of SolarEdge (tests/e2e only). */
static const char * se_api(void) {
    const char * e = getenv("INV_SE_API");
    return (e && *e) ? e : SE_API;
}

static void se_save(void) {
    char path[300], tmp[310];
    path_in_dir(path, sizeof path, SE_STATE);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    FILE * f = fdopen(fd, "w");
    if (!f) { close(fd); return; }
    fprintf(f, "client=%s\nrefresh=%s\naccess=%s\nexp=%ld\nsite=%s\ncode_done=%s\npause_until=%ld\n"
               "power_t=%ld\npower_w=%.0f\ntotal_t=%ld\ntotal_wh=%.0f\n",
            g_se.client, g_se.refresh, g_se.access, (long)g_se.exp, g_se.site, g_se.code_done,
            (long)g_se.pause_until, (long)g_se.power_t, g_se.power_w, (long)g_se.total_t, g_se.total_wh);
    fclose(f);
    rename(tmp, path);
}

static void se_load(void) {
    if (g_se_loaded) return;
    g_se_loaded = 1;
    memset(&g_se, 0, sizeof g_se);
    char path[300];
    path_in_dir(path, sizeof path, SE_STATE);
    FILE * f = fopen(path, "r");
    if (!f) return;
    static char line[4400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char * k = line, * v = eq + 1;
#define SS(key, field) if (!strcmp(k, key)) snprintf(g_se.field, sizeof g_se.field, "%s", v);
        SS("client", client) SS("refresh", refresh) SS("access", access) SS("site", site) SS("code_done", code_done)
#undef SS
        if (!strcmp(k, "exp")) g_se.exp = (time_t)atol(v);
        if (!strcmp(k, "pause_until")) g_se.pause_until = (time_t)atol(v);
        if (!strcmp(k, "power_t")) g_se.power_t = (time_t)atol(v);
        if (!strcmp(k, "total_t")) g_se.total_t = (time_t)atol(v);
        if (!strcmp(k, "power_w")) g_se.power_w = atof(v);
        if (!strcmp(k, "total_wh")) g_se.total_wh = atof(v);
    }
    fclose(f);
}

/* Value of query parameter `key` in s ("...?a=1&code=xyz&site_id=9"), URL-decoded. */
static int query_param(const char * s, const char * key, char * out, size_t n) {
    size_t kl = strlen(key);
    for (const char * p = s; (p = strstr(p, key)) != NULL; p += kl) {
        if (p != s && p[-1] != '?' && p[-1] != '&') continue;
        if (p[kl] != '=') continue;
        const char * v = p + kl + 1;
        size_t o = 0;
        for (; *v && *v != '&' && *v != '#' && !isspace((unsigned char)*v) && o + 1 < n; v++) {
            if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) {
                char hx[3] = { v[1], v[2], 0 };
                out[o++] = (char)strtol(hx, NULL, 16);
                v += 2;
            } else out[o++] = (*v == '+') ? ' ' : *v;
        }
        out[o] = 0;
        return o > 0;
    }
    return 0;
}

/* POST to the token endpoint. 1 = new tokens stored, 0 = failed (msg set,
 * *http = status). */
static int se_token(const char * grant, const char * value, int with_redirect, char * msg, size_t msgn, int * http) {
    static char body[4600];
    char ci[260], cs[260], rd[520];
    static char ev[2200];
    json_esc(g_conf.se_client_id, ci, sizeof ci);
    json_esc(g_conf.se_client_secret, cs, sizeof cs);
    json_esc(value, ev, sizeof ev);
    json_esc(g_conf.se_redirect, rd, sizeof rd);
    int is_code = !strcmp(grant, "authorization_code");
    snprintf(body, sizeof body,
             "{\"grant_type\":\"%s\",\"%s\":\"%s\",\"client_id\":\"%s\",\"client_secret\":\"%s\"%s%s%s}",
             grant, is_code ? "code" : "refresh_token", ev, ci, cs,
             with_redirect ? ",\"redirect_uri\":\"" : "", with_redirect ? rd : "", with_redirect ? "\"" : "");
    const char * h[] = { "Content-Type: application/json", "Accept: application/json", NULL };
    resp_t r;
    *http = 0;
    char turl[300];
    snprintf(turl, sizeof turl, "%s/oauth2/token", se_api());
    if (http_req(turl, h, body, NULL, 0, 25, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    *http = r.code;
    int ok = 0;
    if (r.code == 200) {
        char acc[4096] = "", ref[2048] = "";
        double exp_in = 7200;
        mj_str(r.body, "access_token", acc, sizeof acc);
        mj_str(r.body, "refresh_token", ref, sizeof ref);
        mj_num(r.body, "expires_in", &exp_in);
        if (acc[0]) {
            snprintf(g_se.access, sizeof g_se.access, "%s", acc);
            if (ref[0]) snprintf(g_se.refresh, sizeof g_se.refresh, "%s", ref);
            if (exp_in < 60) exp_in = 60;
            g_se.exp = time(NULL) + (time_t)exp_in;
            snprintf(g_se.client, sizeof g_se.client, "%s", g_conf.se_client_id);
            se_save();
            ok = 1;
        } else snprintf(msg, msgn, "SolarEdge: antwoord zonder access_token");
    } else if (r.code == 0) {
        snprintf(msg, msgn, "SolarEdge niet bereikbaar (%s)", curl_why(r.curl));
    } else {
        char e[120] = "", d[200] = "";
        mj_str(r.body, "error", e, sizeof e);
        mj_str(r.body, "error_description", d, sizeof d);
        if (!d[0]) mj_str(r.body, "message", d, sizeof d);
        snprintf(msg, msgn, "SolarEdge weigert (HTTP %d%s%s%s%s)", r.code, e[0] ? ", " : "", e, d[0] ? ": " : "", d);
        logf_("SolarEdge token (%s): HTTP %d %.200s", grant, r.code, r.body ? r.body : "");
    }
    resp_free(&r);
    return ok;
}

/* A fresh code from the callback file (core) or the pasted address. */
static void se_take_code(char * msg, size_t msgn) {
    char path[300], src[1400] = "";
    path_in_dir(path, sizeof path, "oauth_callback");
    FILE * f = fopen(path, "r");
    int from_file = 0;
    if (f) {
        size_t k = fread(src, 1, sizeof src - 1, f);
        src[k] = 0;
        fclose(f);
        unlink(path);
        from_file = 1;
    } else if (strstr(g_conf.se_auth, "code=")) {
        snprintf(src, sizeof src, "%s", g_conf.se_auth);
    }
    if (!src[0]) return;
    char code[512] = "", site[32] = "", err[120] = "";
    if (!query_param(src, "code", code, sizeof code)) {
        if (query_param(src, "error", err, sizeof err))
            snprintf(msg, msgn, "SolarEdge gaf geen toestemming (%s)", err);
        else if (from_file)
            snprintf(msg, msgn, "terugkeeradres zonder code");
        return;
    }
    if (!strcmp(code, g_se.code_done)) return;        /* already used (codes are single use) */
    query_param(src, "site_id", site, sizeof site);
    snprintf(g_se.code_done, sizeof g_se.code_done, "%s", code);
    se_save();
    int http;
    int ok = se_token("authorization_code", code, 0, msg, msgn, &http);
    if (!ok && http >= 400 && http < 500 && g_conf.se_redirect[0])
        ok = se_token("authorization_code", code, 1, msg, msgn, &http);   /* some servers want redirect_uri */
    if (!ok) { logf_("SolarEdge: linking failed: %s", msg); return; }
    if (site[0]) snprintf(g_se.site, sizeof g_se.site, "%s", site);
    g_se.power_t = g_se.total_t = 0;                  /* read right away */
    g_se.pause_until = 0;
    se_save();
    logf_("SolarEdge: linked (site %s)", g_se.site[0] ? g_se.site : "?");
}

/* A valid access token, refreshed when it (nearly) ran out. */
static int se_access(char * msg, size_t msgn) {
    if (!g_se.refresh[0] && !g_se.access[0]) {
        snprintf(msg, msgn, "nog niet gekoppeld: sla client-ID en secret op en klik op 'Koppelen met SolarEdge'");
        return 0;
    }
    if (g_se.access[0] && time(NULL) < g_se.exp - 300) return 1;
    if (!g_se.refresh[0]) { snprintf(msg, msgn, "koppeling verlopen: klik opnieuw op 'Koppelen met SolarEdge'"); return 0; }
    int http;
    if (se_token("refresh_token", g_se.refresh, 0, msg, msgn, &http)) return 1;
    if (http == 400 || http == 401) {                 /* the grant is gone: link again */
        g_se.refresh[0] = g_se.access[0] = 0;
        se_save();
        snprintf(msg, msgn, "koppeling verlopen of ingetrokken: klik opnieuw op 'Koppelen met SolarEdge'");
    }
    return 0;
}

/* GET SE_API + path. Returns the HTTP status (0 = no answer); body in *r.
 * Handles the credit limit: HTTP 429. */
static int se_get(const char * path, resp_t * r, char * msg, size_t msgn) {
    char url[400];
    static char auth[4200];
    snprintf(url, sizeof url, "%s%s", se_api(), path);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_se.access);
    const char * h[] = { auth, "Accept: application/json", NULL };
    if (http_req(url, h, NULL, NULL, 0, 25, r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    if (r->code == 429) {
        time_t now = time(NULL);
        if (r->body && (strstr(r->body, "credit") || strstr(r->body, "Credit"))) {
            struct tm tm;
            localtime_r(&now, &tm);
            tm.tm_mday = 1; tm.tm_mon += 1; tm.tm_hour = 0; tm.tm_min = 5; tm.tm_sec = 0; tm.tm_isdst = -1;
            g_se.pause_until = mktime(&tm);
            snprintf(msg, msgn, "maandtegoed van de SolarEdge-API is op (weer vanaf de 1e)");
        } else {
            g_se.pause_until = now + 300;
            snprintf(msg, msgn, "SolarEdge: te veel verzoeken, over 5 min opnieuw");
        }
        se_save();
        logf_("SolarEdge: HTTP 429 %.160s", r->body ? r->body : "");
    } else if (r->code == 401) {
        g_se.access[0] = 0;                           /* refresh on the next poll */
        snprintf(msg, msgn, "SolarEdge: toegang geweigerd (HTTP 401)");
    } else if (r->code == 0) {
        snprintf(msg, msgn, "SolarEdge niet bereikbaar (%s)", curl_why(r->curl));
    } else if (r->code != 200) {
        snprintf(msg, msgn, "SolarEdge: HTTP %d", r->code);
        logf_("SolarEdge %s: HTTP %d %.200s", path, r->code, r->body ? r->body : "");
    }
    return r->code;
}

static double se_unit_factor(const char * u) {
    if (!u || !*u) return 1;
    char c = (char)toupper((unsigned char)u[0]);
    if (c == 'K') return 1e3;
    if (c == 'M') return 1e6;
    if (c == 'G') return 1e9;
    return 1;
}

static int poll_solaredge(inv_reading_t * rd, char * msg, size_t msgn) {
    se_load();
    if (!g_conf.se_client_id[0] || !g_conf.se_client_secret[0]) {
        snprintf(msg, msgn, "vul client-ID en secret van je SolarEdge-app in");
        return 0;
    }
    if (g_se.client[0] && strcmp(g_se.client, g_conf.se_client_id)) {   /* another app: start over */
        char keep[512];
        snprintf(keep, sizeof keep, "%s", g_se.code_done);
        memset(&g_se, 0, sizeof g_se);
        snprintf(g_se.code_done, sizeof g_se.code_done, "%s", keep);
        se_save();
    }
    se_take_code(msg, msgn);
    if (msg[0]) return 0;

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int day = lt.tm_hour >= 6 && lt.tm_hour < 22;
    int need_power = day && now - g_se.power_t >= 30 * 60;
    int need_total = (day && now - g_se.total_t >= 2 * 3600) || !g_se.total_t;
    char err[200] = "";

    if ((need_power || need_total) && now >= g_se.pause_until) {
        if (!se_access(msg, msgn)) return 0;
        if (!g_se.site[0]) {
            snprintf(msg, msgn, "site-ID onbekend: koppel opnieuw (het terugkeeradres hoort site_id= te bevatten)");
            return 0;
        }
        char path[200];
        resp_t r;
        if (need_power) {
            char f1[32], t1[32];
            time_t from = now - 2 * 3600;
            struct tm g;
            gmtime_r(&from, &g); strftime(f1, sizeof f1, "%Y-%m-%dT%H%%3A%M%%3A%SZ", &g);
            gmtime_r(&now, &g);  strftime(t1, sizeof t1, "%Y-%m-%dT%H%%3A%M%%3A%SZ", &g);
            snprintf(path, sizeof path, "/sites/%s/power?from=%s&to=%s&resolution=QUARTER_HOUR&unit=W", g_se.site, f1, t1);
            if (se_get(path, &r, err, sizeof err) == 200) {
                char u[16] = "";
                mj_str(r.body, "unit", u, sizeof u);
                double last = -1;
                for (int i = 0; i < 64; i++) {
                    char ip[24];
                    snprintf(ip, sizeof ip, "values[%d]", i);
                    const char * e = mj_find(r.body, ip, NULL);
                    if (!e) break;
                    double v;
                    const char * vp = mj_find(e, "value", NULL);
                    if (vp && *vp != 'n' && mj_num(e, "value", &v)) last = v;
                }
                g_se.power_w = last >= 0 ? last * se_unit_factor(u) : 0;
                g_se.power_t = now;
                se_save();
            }
            resp_free(&r);
        }
        if (need_total && !err[0]) {
            snprintf(path, sizeof path, "/sites/%s/lifetime-energy", g_se.site);
            if (se_get(path, &r, err, sizeof err) == 200) {
                double e;
                char u[16] = "";
                mj_str(r.body, "unit", u, sizeof u);
                if (mj_num(r.body, "energy", &e) && e > 0) {
                    g_se.total_wh = e * se_unit_factor(u);
                    g_se.total_t = now;
                    se_save();
                } else snprintf(err, sizeof err, "SolarEdge: geen totaal in het antwoord");
            }
            resp_free(&r);
        }
    }
    if (!g_se.power_t && !g_se.total_t) {
        snprintf(msg, msgn, "%s", err[0] ? err : (now < g_se.pause_until ? "SolarEdge: even wachten (tegoed)" : "SolarEdge: nog geen meting"));
        return 0;
    }
    if (err[0]) logf_("SolarEdge: %s (last values kept)", err);
    rd->power_w = (day && now - g_se.power_t < 3600) ? g_se.power_w : 0;
    rd->total_wh = g_se.total_t ? g_se.total_wh : -1;
    rd->today_wh = g_se.total_t ? today_from_lifetime(g_se.total_wh) : -1;
    return 1;
}

/* ---------------------------------------------------- GoodWe SEMS+ */
/* GoodWe closed the classic SEMS portal on 2026-09-23; SEMS+ is the way in
 * now. Ported from the solarPanel app's GOODWE1/GOODWE2 v2.1.0 plugin
 * (oepi-loepi & WickedMaster, ToonSoftwareCollective/solarpanel-plugins):
 *   POST semsplus.goodwe.com .../auth/cross-login   pwd = base64(md5hex(pw))
 *   POST eu-gateway .../stations/simple-query        station (by name, else first)
 *   GET  .../stations/device/all-status?stationId=   inverter serial + deviceType
 *   GET  .../equipments/<sn>/telemetry               pAc (kW)
 *   GET  .../equipments/<sn>/telecounting&groups=telecounting_today / _lifetime
 *                                                    proPvStatsToday / proPvStatsTotal (kWh)
 * Every call carries a "token" header (JSON with uid / timestamp / token) and
 * X-Signature = base64(sha256hex(ms@uid@token) + "@" + ms). Logged in once per
 * poll; station and serial are kept until the settings change. */
#define GW_LOGIN "https://semsplus.goodwe.com/web/sems/sems-user/api/v1/auth/cross-login"
#define GW_API   "https://eu-gateway.semsportal.com/web/sems/sems-plant/api"
static char g_gw_station_id[64], g_gw_sn[64], g_gw_type[32];
/* INV_GW_LOGIN / INV_GW_API: a test server instead of GoodWe (tests only). */
static const char * gw_login_url(void) { const char * e = getenv("INV_GW_LOGIN"); return (e && *e) ? e : GW_LOGIN; }
static const char * gw_api(void) { const char * e = getenv("INV_GW_API"); return (e && *e) ? e : GW_API; }

static void gw_ms(char * out, size_t n) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(out, n, "%lld", (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000);
}
static void gw_headers(const char * uid, const char * ts, const char * tok, char * h_tok, size_t tn, char * h_sig, size_t sn) {
    char ms[24], in[400], hex[65], cat[100];
    uint8_t d[32];
    gw_ms(ms, sizeof ms);
    snprintf(in, sizeof in, "%s@%s@%s", ms, uid, tok);
    inv_sha256(in, strlen(in), d);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    snprintf(cat, sizeof cat, "%s@%s", hex, ms);
    char b64[160];
    inv_b64((const uint8_t *)cat, strlen(cat), b64);
    snprintf(h_tok, tn, "token: {\"uid\":\"%s\",\"timestamp\":%s,\"token\":\"%s\",\"client\":\"semsPlusWeb\",\"version\":\"\",\"language\":\"en\"}",
             uid, ts[0] ? ts : "0", tok);
    snprintf(h_sig, sn, "X-Signature: %s", b64);
}
/* "code" of a SEMS+ answer is ok ("00000" / "0" / "200"). */
static int gw_ok(const char * body) {
    char c[16] = "";
    mj_str(body, "code", c, sizeof c);
    return !strcmp(c, "00000") || !strcmp(c, "0") || !strcmp(c, "200");
}
/* value of factor `code` in a factors answer (data[g].factors[f]) */
static int gw_factor(const char * body, const char * code, double * v) {
    for (int g = 0; g < 16; g++) {
        char p[48];
        snprintf(p, sizeof p, "data[%d]", g);
        const char * grp = mj_find(body, p, NULL);
        if (!grp) break;
        for (int f = 0; f < 64; f++) {
            char fp[32], c[48] = "";
            snprintf(fp, sizeof fp, "factors[%d]", f);
            const char * fac = mj_find(grp, fp, NULL);
            if (!fac) break;
            if (mj_str(fac, "code", c, sizeof c) && !strcmp(c, code)) {
                const char * dv = mj_find(fac, "data", NULL);
                if (!dv || *dv == 'n') return 0;
                return mj_num(fac, "data", v);
            }
        }
    }
    return 0;
}
static int gw_call(const char * url, const char * body, const char * uid, const char * ts, const char * tok,
                   resp_t * r, char * msg, size_t msgn) {
    char h_tok[600], h_sig[200];
    gw_headers(uid, ts, tok, h_tok, sizeof h_tok, h_sig, sizeof h_sig);
    const char * h[] = { "Content-Type: application/json; charset=utf-8", "Accept: application/json, text/plain, */*",
                         "Origin: https://semsplus.goodwe.com", "Referer: https://semsplus.goodwe.com/",
                         "User-Agent: Mozilla/5.0 (X11; Linux) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
                         h_tok, h_sig, "neutral: 0", "currentLang: en", NULL };
    if (http_req(url, h, body, NULL, 0, 25, r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    if (r->code == 0) { snprintf(msg, msgn, "SEMS+ niet bereikbaar (%s)", curl_why(r->curl)); return 0; }
    if (r->code != 200) { snprintf(msg, msgn, "SEMS+: HTTP %d", r->code); return 0; }
    if (!gw_ok(r->body)) {
        char c[16] = "", m[120] = "";
        mj_str(r->body, "code", c, sizeof c);
        mj_str(r->body, "msg", m, sizeof m);
        if (!m[0]) mj_str(r->body, "message", m, sizeof m);
        snprintf(msg, msgn, "SEMS+ weigert (%s%s%s)", c, m[0] ? ": " : "", m);
        return 0;
    }
    return 1;
}

static int poll_goodwe(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!g_conf.gw_user[0] || !g_conf.gw_pass[0]) {
        snprintf(msg, msgn, "vul e-mail en wachtwoord van je SEMS+-account in");
        return 0;
    }
    /* login: pwd = base64 of the lowercase hex MD5 of the password */
    uint8_t md[16];
    char mdhex[33], pwd[64], body[600], eu[300], ep[200];
    inv_md5(g_conf.gw_pass, strlen(g_conf.gw_pass), md);
    for (int i = 0; i < 16; i++) snprintf(mdhex + 2 * i, 3, "%02x", md[i]);
    inv_b64((const uint8_t *)mdhex, 32, pwd);
    json_esc(g_conf.gw_user, eu, sizeof eu);
    json_esc(pwd, ep, sizeof ep);
    snprintf(body, sizeof body, "{\"account\":\"%s\",\"pwd\":\"%s\",\"agreement\":1,\"isChinese\":false,\"isLocal\":false}", eu, ep);
    resp_t r;
    if (!gw_call(gw_login_url(), body, "", "", "", &r, msg, msgn)) {
        if (strstr(msg, "weigert")) logf_("GoodWe login: %.200s", r.body ? r.body : "");
        resp_free(&r);
        return 0;
    }
    char uid[96] = "", tok[512] = "", ts[32] = "";
    mj_str(r.body, "data.uid", uid, sizeof uid);
    mj_str(r.body, "data.token", tok, sizeof tok);
    mj_str(r.body, "data.timestamp", ts, sizeof ts);
    resp_free(&r);
    if (!uid[0] || !tok[0]) { snprintf(msg, msgn, "SEMS+: login zonder token"); return 0; }

    char url[400];
    if (!g_gw_station_id[0]) {
        snprintf(url, sizeof url, "%s/stations/simple-query", gw_api());
        if (!gw_call(url, "{\"pageIndex\":1,\"pageSize\":20}", uid, ts, tok, &r, msg, msgn)) {
            resp_free(&r); return 0;
        }
        char first[64] = "";
        for (int i = 0; i < 20; i++) {
            char p[48], id[64] = "", nm[96] = "";
            snprintf(p, sizeof p, "data.dataList[%d]", i);
            const char * st = mj_find(r.body, p, NULL);
            if (!st) break;
            mj_str(st, "id", id, sizeof id);
            mj_str(st, "stationName", nm, sizeof nm);
            if (!first[0]) snprintf(first, sizeof first, "%s", id);
            if (g_conf.gw_station[0] && !strcmp(nm, g_conf.gw_station)) { snprintf(g_gw_station_id, sizeof g_gw_station_id, "%s", id); break; }
        }
        resp_free(&r);
        if (!g_gw_station_id[0]) {
            if (!first[0]) { snprintf(msg, msgn, "SEMS+: geen installatie op dit account"); return 0; }
            if (g_conf.gw_station[0]) logf_("GoodWe: no installation named '%s', using the first one", g_conf.gw_station);
            snprintf(g_gw_station_id, sizeof g_gw_station_id, "%s", first);
        }
        logf_("GoodWe: installation %s", g_gw_station_id);
    }
    if (!g_gw_sn[0]) {
        snprintf(url, sizeof url, "%s/stations/device/all-status?stationId=%s", gw_api(), g_gw_station_id);
        if (!gw_call(url, NULL, uid, ts, tok, &r, msg, msgn)) { resp_free(&r); return 0; }
        /* an INVERTER group first, else any group with a device */
        for (int pass = 0; pass < 2 && !g_gw_sn[0]; pass++)
            for (int g = 0; g < 16 && !g_gw_sn[0]; g++) {
                char p[64], ty[32] = "";
                snprintf(p, sizeof p, "data.deviceDetailList[%d]", g);
                const char * grp = mj_find(r.body, p, NULL);
                if (!grp) break;
                mj_str(grp, "deviceType", ty, sizeof ty);
                if (pass == 0 && strcmp(ty, "INVERTER")) continue;
                for (int st = 0; st < 8 && !g_gw_sn[0]; st++) {
                    char sp[48], sn[64] = "";
                    snprintf(sp, sizeof sp, "statusDetailList[%d].snList[0]", st);
                    if (mj_str(grp, sp, sn, sizeof sn) && sn[0]) {
                        snprintf(g_gw_sn, sizeof g_gw_sn, "%s", sn);
                        snprintf(g_gw_type, sizeof g_gw_type, "%s", ty);
                    }
                }
            }
        resp_free(&r);
        if (!g_gw_sn[0]) { snprintf(msg, msgn, "SEMS+: geen omvormer in de installatie"); return 0; }
        logf_("GoodWe: device %s (%s)", g_gw_sn, g_gw_type);
    }
    double v;
    snprintf(url, sizeof url, "%s/equipments/%s/telemetry?deviceType=%s&pwId=%s", gw_api(), g_gw_sn, g_gw_type, g_gw_station_id);
    if (!gw_call(url, NULL, uid, ts, tok, &r, msg, msgn)) { resp_free(&r); return 0; }
    rd->power_w = gw_factor(r.body, "pAc", &v) ? v * 1000.0 : 0;
    resp_free(&r);
    /* one "groups" value per call: combined today + lifetime is refused (A0400) */
    snprintf(url, sizeof url, "%s/equipments/%s/telecounting?deviceType=%s&pwId=%s&groups=telecounting_today",
             gw_api(), g_gw_sn, g_gw_type, g_gw_station_id);
    if (gw_call(url, NULL, uid, ts, tok, &r, msg, msgn) && gw_factor(r.body, "proPvStatsToday", &v)) rd->today_wh = v * 1000.0;
    resp_free(&r);
    msg[0] = 0;
    snprintf(url, sizeof url, "%s/equipments/%s/telecounting?deviceType=%s&pwId=%s&groups=telecounting_lifetime",
             gw_api(), g_gw_sn, g_gw_type, g_gw_station_id);
    if (gw_call(url, NULL, uid, ts, tok, &r, msg, msgn) && gw_factor(r.body, "proPvStatsTotal", &v)) rd->total_wh = v * 1000.0;
    resp_free(&r);
    msg[0] = 0;
    return 1;
}

/* ------------------------------------- solarPanel app plugins (local) */
/* Ported from ToonSoftwareCollective/solarpanel-plugins (oepi-loepi and
 * contributors): the same requests and fields, one GET (or two) per poll. */

/* Number after the n-th (1-based) occurrence of mark; 0 when missing. */
static double num_after(const char * body, const char * mark, int n) {
    const char * p = body;
    for (int i = 0; i < n; i++) {
        p = strstr(p, mark);
        if (!p) return 0;
        p += strlen(mark);
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    char * e;
    double v = strtod(p, &e);
    return e == p ? 0 : v;
}
/* Number inside <tag>...</tag>; -1 when the tag is missing. */
static double xml_num(const char * body, const char * tag) {
    char open[48];
    snprintf(open, sizeof open, "<%s>", tag);
    const char * p = strstr(body, open);
    if (!p) return -1;
    return strtod(p + strlen(open), NULL);
}
/* GET http://<host><path> (no TLS); 1 = 200 with a body. */
static int local_get(const char * path, const char * userpass, resp_t * r, char * msg, size_t msgn) {
    char url[300];
    snprintf(url, sizeof url, "http://%s%s", g_conf.host, path);
    if (http_req(url, NULL, NULL, userpass, 0, 15, r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    if (r->code == 0) { snprintf(msg, msgn, "geen verbinding met %s (%s)", g_conf.host, curl_why(r->curl)); return 0; }
    if (r->code != 200) { snprintf(msg, msgn, "%s: HTTP %d", g_conf.host, r->code); return 0; }
    return 1;
}

/* Fronius Solar API v1 (FRONIUS1): PAC (W) only while StatusCode 7 (running). */
static int poll_fronius(inv_reading_t * rd, char * msg, size_t msgn) {
    resp_t r;
    int ok = local_get("/solar_api/v1/GetInverterRealtimeData.cgi?Scope=Device&DeviceID=1&DataCollection=CommonInverterData",
                       NULL, &r, msg, msgn);
    if (ok) {
        double code = -1, st = 0, v;
        mj_num(r.body, "Head.Status.Code", &code);
        if (code != 0) { snprintf(msg, msgn, "Fronius meldt status %.0f", code); ok = 0; }
        else {
            mj_num(r.body, "Body.Data.DeviceStatus.StatusCode", &st);
            rd->power_w = (st == 7 && mj_num(r.body, "Body.Data.PAC.Value", &v)) ? v : 0;
            if (mj_num(r.body, "Body.Data.TOTAL_ENERGY.Value", &v)) rd->total_wh = v;
            if (mj_num(r.body, "Body.Data.DAY_ENERGY.Value", &v)) rd->today_wh = v;
        }
    }
    resp_free(&r);
    return ok;
}
/* Kostal Piko web page (KOSTAL1): values after 'bgcolor="#FFFFFF">':
 * 1 = power W, 2 = total kWh, 3 = today kWh; login pvserver:pvwr. */
static int poll_kostal(inv_reading_t * rd, char * msg, size_t msgn) {
    resp_t r;
    int ok = local_get("/", "pvserver:pvwr", &r, msg, msgn);
    if (ok) {
        const char * m = "bgcolor=\"#FFFFFF\">";
        if (!strstr(r.body, m)) { snprintf(msg, msgn, "Kostal: onbekende pagina"); ok = 0; }
        else {
            rd->power_w  = num_after(r.body, m, 1);
            rd->total_wh = num_after(r.body, m, 2) * 1000.0;
            rd->today_wh = num_after(r.body, m, 3) * 1000.0;
        }
    }
    resp_free(&r);
    return ok;
}
/* ZeverSolar home.cgi (ZEVER1): line 11 W, line 12 today kWh, line 13 "OK".
 * "1.5" means 1.05 kWh (one decimal digit = hundredths). No lifetime total. */
static int poll_zever(inv_reading_t * rd, char * msg, size_t msgn) {
    resp_t r;
    int ok = local_get("/home.cgi", NULL, &r, msg, msgn);
    if (ok) {
        char * line[16] = { 0 };
        int n = 0;
        for (char * p = r.body; p && n < 16; ) {
            line[n++] = p;
            char * nl = strchr(p, '\n');
            if (nl) { *nl = 0; if (nl > p && nl[-1] == '\r') nl[-1] = 0; p = nl + 1; } else p = NULL;
        }
        if (n < 13) { snprintf(msg, msgn, "ZeverSolar: onbekend antwoord"); ok = 0; }
        else if (strncmp(line[12], "OK", 2)) { rd->power_w = 0; rd->today_wh = -1; }   /* asleep / no grid */
        else {
            rd->power_w = atof(line[10]);
            const char * t = line[11];
            const char * dot = strchr(t, '.');
            double kwh = atof(t);
            if (dot && strlen(dot + 1) == 1) kwh = atoi(t) + atoi(dot + 1) / 100.0;
            rd->today_wh = kwh * 1000.0;
        }
    }
    resp_free(&r);
    return ok;
}
/* openDTU (OPENDTU1): total.Power.v W, YieldDay.v Wh, YieldTotal.v kWh. */
static int poll_opendtu(inv_reading_t * rd, char * msg, size_t msgn) {
    resp_t r;
    int ok = local_get("/api/livedata/status", NULL, &r, msg, msgn);
    if (ok) {
        double v;
        if (!mj_num(r.body, "total.Power.v", &v)) { snprintf(msg, msgn, "openDTU: geen total.Power in het antwoord"); ok = 0; }
        else {
            rd->power_w = v;
            if (mj_num(r.body, "total.YieldDay.v", &v)) rd->today_wh = v;
            if (mj_num(r.body, "total.YieldTotal.v", &v)) rd->total_wh = v * 1000.0;
        }
    }
    resp_free(&r);
    return ok;
}
/* SAJ local real_time_data.xml (SAJ1): p-ac W, e-today / e-total kWh. */
static int poll_saj(inv_reading_t * rd, char * msg, size_t msgn) {
    resp_t r;
    int ok = local_get("/real_time_data.xml", NULL, &r, msg, msgn);
    if (ok) {
        double p = xml_num(r.body, "p-ac"), d = xml_num(r.body, "e-today"), t = xml_num(r.body, "e-total");
        if (p < 0) { snprintf(msg, msgn, "SAJ: geen p-ac in het antwoord"); ok = 0; }
        else {
            rd->power_w = p;
            if (d >= 0) rd->today_wh = d * 1000.0;
            if (t >= 0) rd->total_wh = t * 1000.0;
        }
    }
    resp_free(&r);
    return ok;
}
/* FP4ALL datalogger status.xml (FP4ALL): gauge_power W, energy_today /
 * energy_total as the plugin uses them (Wh). */
static int poll_fp4all(inv_reading_t * rd, char * msg, size_t msgn) {
    resp_t r;
    int ok = local_get("/status.xml", NULL, &r, msg, msgn);
    if (ok) {
        double p = xml_num(r.body, "gauge_power"), d = xml_num(r.body, "energy_today"), t = xml_num(r.body, "energy_total");
        if (p < 0) { snprintf(msg, msgn, "FP4ALL: geen gauge_power in het antwoord"); ok = 0; }
        else {
            rd->power_w = p;
            if (d >= 0) rd->today_wh = d;
            if (t >= 0) rd->total_wh = t;
        }
    }
    resp_free(&r);
    return ok;
}
/* SMA webconnect (SMA1 http / SMA3 https): login as "usr" for a sid, then
 * getValues 6100_40263F00 (W), 6400_00262200 (today Wh), 6400_00260100
 * (total Wh). An SMA allows only a few sessions: the sid is kept and only
 * renewed when refused (the plugin logged in on every poll). */
static char g_sma_sid[64];
static int  g_sma_https = -1;           /* -1 unknown, 0 http, 1 https (self-signed) */
static int sma_post(const char * path, const char * body, resp_t * r) {
    char url[300];
    snprintf(url, sizeof url, "%s://%s%s", g_sma_https ? "https" : "http", g_conf.host, path);
    const char * h[] = { "Accept: application/json, text/plain, */*", "Content-Type: application/json;charset=UTF-8", NULL };
    return http_req(url, h, body, NULL, g_sma_https ? 1 : 0, 15, r);
}
static int sma_val(const char * body, const char * key, double * v) {
    /* result.<device id>.<key>["1"][0].val, the device id is the first key of result */
    const char * res = mj_find(body, "result", NULL);
    if (!res || *res != '{') return 0;
    const char * q = strchr(res, '"');
    if (!q) return 0;
    const char * qe = strchr(q + 1, '"');
    if (!qe || qe - q - 1 >= 48) return 0;
    char path[128], dev[48];
    memcpy(dev, q + 1, (size_t)(qe - q - 1));
    dev[qe - q - 1] = 0;
    snprintf(path, sizeof path, "%s.%s.1[0].val", dev, key);
    const char * vp = mj_find(res, path, NULL);
    if (!vp || *vp == 'n') return 0;
    return mj_num(res, path, v);
}
static int poll_sma(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!g_conf.sma_pass[0]) { snprintf(msg, msgn, "vul het wachtwoord (gebruiker) van de SMA in"); return 0; }
    resp_t r;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!g_sma_sid[0]) {
            char ep[160], body[220];
            json_esc(g_conf.sma_pass, ep, sizeof ep);
            snprintf(body, sizeof body, "{\"pass\":\"%s\",\"right\":\"usr\"}", ep);
            int tries[2] = { g_sma_https < 0 ? 1 : g_sma_https, 0 };     /* unknown: https first, then http */
            int n = g_sma_https < 0 ? 2 : 1, got = 0;
            for (int i = 0; i < n && !got; i++) {
                g_sma_https = tries[i];
                if (sma_post("/dyn/login.json", body, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
                if (r.code == 200 && mj_str(r.body, "result.sid", g_sma_sid, sizeof g_sma_sid) && g_sma_sid[0] && strcmp(g_sma_sid, "null"))
                    got = 1;
                else if (r.code == 200) {
                    double err = 0;
                    mj_num(r.body, "err", &err);
                    snprintf(msg, msgn, err ? "SMA weigert de login (fout %.0f: wachtwoord? te veel sessies?)" : "SMA: login zonder sessie", err);
                    g_sma_sid[0] = 0;
                    resp_free(&r);
                    return 0;
                }
                resp_free(&r);
            }
            if (!got) { g_sma_https = -1; snprintf(msg, msgn, "geen verbinding met SMA %s", g_conf.host); return 0; }
            logf_("SMA: logged in via %s", g_sma_https ? "https" : "http");
        }
        char path[128];
        snprintf(path, sizeof path, "/dyn/getValues.json?sid=%s", g_sma_sid);
        if (sma_post(path, "{\"destDev\":[],\"keys\":[\"6400_00260100\",\"6400_00262200\",\"6100_40263F00\"]}", &r) != 0) {
            snprintf(msg, msgn, "curl ontbreekt"); return 0;
        }
        if (r.code == 200 && mj_find(r.body, "result", NULL)) {
            double v;
            rd->power_w = sma_val(r.body, "6100_40263F00", &v) ? v : 0;     /* null at night */
            if (sma_val(r.body, "6400_00262200", &v)) rd->today_wh = v;
            if (sma_val(r.body, "6400_00260100", &v)) rd->total_wh = v;
            resp_free(&r);
            return 1;
        }
        resp_free(&r);
        g_sma_sid[0] = 0;                                    /* session gone: log in again */
    }
    snprintf(msg, msgn, "SMA: geen waarden (sessie verlopen?)");
    return 0;
}
/* Domoticz (DOM-DOUBLE1/2): one idx or two ("12+19") added up; Usage W,
 * CounterToday / Data kWh. New API (2023.2+) first, then the old one. */
static int dz_one(const char * idx, double * w, double * today, double * total, char * msg, size_t msgn) {
    static const char * const paths[2] = { "/json.htm?type=command&param=getdevices&rid=%s", "/json.htm?type=devices&rid=%s" };
    for (int k = 0; k < 2; k++) {
        char path[96];
        snprintf(path, sizeof path, paths[k], idx);
        resp_t r;
        int ok = local_get(path, NULL, &r, msg, msgn);
        if (ok && mj_find(r.body, "result[0]", NULL)) {
            char u[48] = "", ct[48] = "", d[48] = "";
            mj_str(r.body, "result[0].Usage", u, sizeof u);
            mj_str(r.body, "result[0].CounterToday", ct, sizeof ct);
            mj_str(r.body, "result[0].Data", d, sizeof d);
            *w += atof(u);
            *today += atof(ct) * 1000.0;
            *total += atof(d) * 1000.0;
            resp_free(&r);
            return 1;
        }
        resp_free(&r);
        if (!ok && strstr(msg, "geen verbinding")) return 0;
    }
    snprintf(msg, msgn, "Domoticz: apparaat idx %s niet gevonden", idx);
    return 0;
}
static int poll_domoticz(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!g_conf.dz_idx[0]) { snprintf(msg, msgn, "vul de IDX van het zonne-apparaat in Domoticz in"); return 0; }
    char a[24] = "", b[24] = "";
    const char * plus = strchr(g_conf.dz_idx, '+');
    snprintf(a, sizeof a, "%.*s", plus ? (int)(plus - g_conf.dz_idx) : (int)strlen(g_conf.dz_idx), g_conf.dz_idx);
    if (plus) snprintf(b, sizeof b, "%s", plus + 1);
    for (char * c = a; *c; c++) if (!isdigit((unsigned char)*c)) { *c = 0; break; }
    for (char * c = b; *c; c++) if (!isdigit((unsigned char)*c)) { *c = 0; break; }
    double w = 0, d = 0, t = 0;
    if (!a[0] || !dz_one(a, &w, &d, &t, msg, msgn)) return 0;
    if (b[0] && !dz_one(b, &w, &d, &t, msg, msgn)) return 0;
    rd->power_w = w; rd->today_wh = d; rd->total_wh = t;
    return 1;
}

/* ------------------------------------- solarPanel app plugins (cloud API key) */
/* PVOutput (PVOUTPUT1): getstatus (fields 0 / 1 as the plugin reads them:
 * today Wh, power W) + getstatistic (field 0: lifetime Wh). 60 calls an hour
 * at most: polled every 5 min. */
static int pvo_csv(const char * path, double * f0, double * f1, char * msg, size_t msgn) {
    char url[160], hk[140], hs[60];
    const char * base = getenv("INV_PVO_API");          /* test server (tests only) */
    snprintf(url, sizeof url, "%s/%s", (base && *base) ? base : "https://pvoutput.org/service/r2", path);
    snprintf(hk, sizeof hk, "X-Pvoutput-Apikey: %s", g_conf.pvo_key);
    snprintf(hs, sizeof hs, "X-Pvoutput-SystemId: %s", g_conf.pvo_sid);
    const char * h[] = { hk, hs, NULL };
    resp_t r;
    if (http_req(url, h, "", NULL, 0, 20, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    int ok = r.code == 200;
    if (ok) {
        char * e;
        *f0 = strtod(r.body, &e);
        if (f1) *f1 = (*e == ',') ? strtod(e + 1, NULL) : 0;
    } else if (r.code == 0) snprintf(msg, msgn, "PVOutput niet bereikbaar (%s)", curl_why(r.curl));
    else snprintf(msg, msgn, "PVOutput: HTTP %d %.60s", r.code, r.body ? r.body : "");
    resp_free(&r);
    return ok;
}
static int poll_pvoutput(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!g_conf.pvo_key[0] || !g_conf.pvo_sid[0]) { snprintf(msg, msgn, "vul API-sleutel en systeem-ID van PVOutput in"); return 0; }
    double a, b, t;
    if (!pvo_csv("getstatus.jsp?stats=1", &a, &b, msg, msgn)) return 0;
    rd->today_wh = a; rd->power_w = b;
    if (pvo_csv("getstatistic.jsp", &t, NULL, msg, msgn)) rd->total_wh = t;
    msg[0] = 0;
    return 1;
}
/* SolaxCloud (SOLAX1): getRealtimeInfo with tokenId + sn: acpower W,
 * yieldtoday / yieldtotal kWh. Polled every 5 min. */
static int poll_solax(inv_reading_t * rd, char * msg, size_t msgn) {
    if (!g_conf.solax_token[0] || !g_conf.solax_sn[0]) { snprintf(msg, msgn, "vul token-ID en registratienummer van Solax in"); return 0; }
    char tk[200], sn[100], url[400];
    url_enc(g_conf.solax_token, tk, sizeof tk);
    url_enc(g_conf.solax_sn, sn, sizeof sn);
    const char * base = getenv("INV_SOLAX_API");        /* test server (tests only) */
    snprintf(url, sizeof url, "%s/getRealtimeInfo.do?tokenId=%s&sn=%s",
             (base && *base) ? base : "https://www.solaxcloud.com:9443/proxy/api", tk, sn);
    resp_t r;
    if (http_req(url, NULL, NULL, NULL, 0, 20, &r) != 0) { snprintf(msg, msgn, "curl ontbreekt"); return 0; }
    int ok = 0;
    if (r.code == 200 && mj_bool(r.body, "success")) {
        double v;
        rd->power_w = mj_num(r.body, "result.acpower", &v) ? v : 0;
        if (mj_num(r.body, "result.yieldtoday", &v)) rd->today_wh = v * 1000.0;
        if (mj_num(r.body, "result.yieldtotal", &v)) rd->total_wh = v * 1000.0;
        ok = 1;
    } else if (r.code == 200) {
        char e[120] = "";
        mj_str(r.body, "exception", e, sizeof e);
        snprintf(msg, msgn, "Solax weigert (%s)", e[0] ? e : "token / registratienummer?");
    } else if (r.code == 0) snprintf(msg, msgn, "SolaxCloud niet bereikbaar (%s)", curl_why(r.curl));
    else snprintf(msg, msgn, "SolaxCloud: HTTP %d", r.code);
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
            g_pj_ever = 0;
            g_frz_since = 0;
            g_gw_station_id[0] = g_gw_sn[0] = g_gw_type[0] = 0;
            g_sma_sid[0] = 0; g_sma_https = -1;
            last_ok = -1;
            zero_polls = 0;
        }
        inv_reading_t rd = { 0, -1, -1 };
        char msg[200] = "";
        int ok = 0, cloud = 0;
        const char * t = g_conf.type;
        const inv_mb_profile_t * mbp = inv_mb_profile(t);
        int local = !strcmp(t, "enphase") || !strcmp(t, "solis_local") || !strcmp(t, "omnik") || !strcmp(t, "sunspec") || mbp
                 || !strcmp(t, "fronius") || !strcmp(t, "kostal_piko") || !strcmp(t, "zeversolar") || !strcmp(t, "opendtu")
                 || !strcmp(t, "saj_local") || !strcmp(t, "fp4all") || !strcmp(t, "sma_web") || !strcmp(t, "domoticz");
        if (!t[0]) snprintf(msg, sizeof msg, "geen omvormer ingesteld");
        else if (local && !host_ok(g_conf.host)) snprintf(msg, sizeof msg, "vul het IP-adres in");
        else if (!strcmp(t, "enphase"))     ok = poll_enphase(&rd, msg, sizeof msg);
        else if (!strcmp(t, "solis_local")) ok = poll_stick(0, &rd, msg, sizeof msg);
        else if (!strcmp(t, "omnik"))       ok = poll_stick(1, &rd, msg, sizeof msg);
        else if (!strcmp(t, "sunspec"))     ok = poll_sunspec(&rd, msg, sizeof msg);
        else if (mbp)                       ok = poll_mbprofile(mbp, &rd, msg, sizeof msg);
        else if (!strcmp(t, "soliscloud")) { cloud = 1; ok = poll_soliscloud(&rd, msg, sizeof msg); }
        else if (!strcmp(t, "solaredge_cloud")) { cloud = 1; ok = poll_solaredge(&rd, msg, sizeof msg); }
        else if (!strcmp(t, "goodwe_sems")) { cloud = 2; ok = poll_goodwe(&rd, msg, sizeof msg); }
        else if (!strcmp(t, "fronius"))     ok = poll_fronius(&rd, msg, sizeof msg);
        else if (!strcmp(t, "kostal_piko")) ok = poll_kostal(&rd, msg, sizeof msg);
        else if (!strcmp(t, "zeversolar"))  ok = poll_zever(&rd, msg, sizeof msg);
        else if (!strcmp(t, "opendtu"))     ok = poll_opendtu(&rd, msg, sizeof msg);
        else if (!strcmp(t, "saj_local"))   ok = poll_saj(&rd, msg, sizeof msg);
        else if (!strcmp(t, "fp4all"))      ok = poll_fp4all(&rd, msg, sizeof msg);
        else if (!strcmp(t, "sma_web"))     ok = poll_sma(&rd, msg, sizeof msg);
        else if (!strcmp(t, "domoticz"))    ok = poll_domoticz(&rd, msg, sizeof msg);
        else if (!strcmp(t, "pvoutput"))    { cloud = 2; ok = poll_pvoutput(&rd, msg, sizeof msg); }
        else if (!strcmp(t, "solax_cloud")) { cloud = 2; ok = poll_solax(&rd, msg, sizeof msg); }
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
        if (cloud == 2 && wait < 300) wait = 300;    /* SEMS+ refreshes every ~5 min */
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
                path_in_dir(path, sizeof path, "oauth_callback");   /* SolarEdge link: act now */
                if (stat(path, &st) == 0) break;
            }
        }
    }
    logf_("stopped");
    return 0;
}
