/* Inverter response parsers - see inv_drivers.h. */
#include "inv_drivers.h"
#include "inv_crypto.h"
#include "minijson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void reading_init(inv_reading_t * r) {
    r->power_w = 0;
    r->today_wh = -1;
    r->total_wh = -1;
}

/* ------------------------------------------------------------- Enphase */
int inv_parse_envoy_v1(const char * body, inv_reading_t * r) {
    reading_init(r);
    double v;
    if (!mj_num(body, "wattsNow", &v)) return 0;
    r->power_w = v < 0 ? 0 : v;          /* the Envoy reports small negatives at night */
    if (mj_num(body, "wattHoursToday", &v))    r->today_wh = v;
    if (mj_num(body, "wattHoursLifetime", &v)) r->total_wh = v;
    return 1;
}

int inv_parse_envoy_serial(const char * body, char * out, size_t n) {
    const char * p = body ? strstr(body, "<sn>") : NULL;
    if (!p) return 0;
    p += 4;
    size_t L = strcspn(p, "<");
    if (!L || L >= n) return 0;
    memcpy(out, p, L);
    out[L] = 0;
    for (char * c = out; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) return 0;
    return 1;
}

/* ------------------------------------------------- WiFi stick (Solis / Omnik) */
/* Value of `var <name> = "...";` (spaces optional). 1 = found (may be empty). */
static int js_var(const char * body, const char * name, char * out, size_t n) {
    const char * p = body;
    size_t nl = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        const char * q = p + nl;
        p = q;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"' && *q != '\'') continue;
        char quote = *q++;
        size_t L = 0;
        while (q[L] && q[L] != quote && q[L] != '\n') L++;
        if (L >= n) L = n - 1;
        memcpy(out, q, L);
        out[L] = 0;
        /* trim */
        char * s = out;
        while (*s == ' ') s++;
        memmove(out, s, strlen(s) + 1);
        for (L = strlen(out); L && out[L - 1] == ' '; ) out[--L] = 0;
        return 1;
    }
    return 0;
}

static int to_num(const char * s, double * v) {
    if (!s || !*s) return 0;
    char * e;
    *v = strtod(s, &e);
    return e != s;
}

int inv_parse_stick_html(const char * body, inv_reading_t * r) {
    reading_init(r);
    char now[32], today[32], total[32];
    if (!body || !js_var(body, "webdata_now_p", now, sizeof now)) return 0;
    double v;
    int have_total = js_var(body, "webdata_total_e", total, sizeof total) && to_num(total, &v);
    if (have_total) r->total_wh = v * 1000.0;
    if (js_var(body, "webdata_today_e", today, sizeof today) && to_num(today, &v)) r->today_wh = v * 1000.0;
    if (to_num(now, &v)) r->power_w = v < 0 ? 0 : v;
    else if (!have_total) return 0;      /* empty page: no data at all */
    return 1;
}

int inv_parse_stick_js(const char * body, inv_reading_t * r) {
    reading_init(r);
    if (!body) return 0;
    static const char * const names[] = { "webData", "myDeviceArray[0]" };
    char csv[512] = "";
    for (int i = 0; i < 2 && !csv[0]; i++) {
        const char * p = strstr(body, names[i]);
        if (!p) continue;
        p = strchr(p, '"');
        if (!p) continue;
        p++;
        size_t L = strcspn(p, "\"");
        if (L >= sizeof csv) L = sizeof csv - 1;
        memcpy(csv, p, L);
        csv[L] = 0;
    }
    if (!csv[0]) return 0;
    char * f[10] = {0};
    int nf = 0;
    for (char * s = csv; nf < 10; ) {
        f[nf++] = s;
        char * c = strchr(s, ',');
        if (!c) break;
        *c = 0;
        s = c + 1;
    }
    double v;
    if (nf < 8 || !to_num(f[5], &v)) return 0;
    r->power_w = v < 0 ? 0 : v;
    if (to_num(f[6], &v)) r->today_wh = v * 10.0;     /* 0.01 kWh */
    if (to_num(f[7], &v)) r->total_wh = v * 100.0;    /* 0.1 kWh */
    return 1;
}

/* ----------------------------------------------------------- SolisCloud */
static double unit_mult(const char * u, double dflt) {
    if (!u || !*u) return dflt;
    if (!strcasecmp(u, "W") || !strcasecmp(u, "Wh")) return 1.0;
    if (!strcasecmp(u, "kW") || !strcasecmp(u, "kWh")) return 1e3;
    if (!strcasecmp(u, "MW") || !strcasecmp(u, "MWh")) return 1e6;
    if (!strcasecmp(u, "GW") || !strcasecmp(u, "GWh")) return 1e9;
    return dflt;
}

int inv_parse_soliscloud(const char * body, inv_reading_t * r, char * err, size_t errn) {
    reading_init(r);
    if (err && errn) err[0] = 0;
    char code[16] = "";
    mj_str(body, "code", code, sizeof code);
    if (!mj_bool(body, "success") || (code[0] && strcmp(code, "0"))) {
        if (err && errn && !mj_str(body, "msg", err, errn)) snprintf(err, errn, "onverwacht antwoord");
        return 0;
    }
    const char * rec = mj_find(body, "data.page.records[0]", NULL);
    if (!rec) {
        if (err && errn) snprintf(err, errn, "geen installatie (station) in dit account");
        return 0;
    }
    double v;
    char u[16];
    if (!mj_num(rec, "power", &v)) {
        if (err && errn) snprintf(err, errn, "geen vermogen in het antwoord");
        return 0;
    }
    u[0] = 0; mj_str(rec, "powerStr", u, sizeof u);
    r->power_w = v * unit_mult(u, 1e3);
    if (r->power_w < 0) r->power_w = 0;
    if (mj_num(rec, "dayEnergy", &v)) {
        u[0] = 0; mj_str(rec, "dayEnergyStr", u, sizeof u);
        r->today_wh = v * unit_mult(u, 1e3);
    }
    if (mj_num(rec, "allEnergy", &v)) {
        u[0] = 0; mj_str(rec, "allEnergyStr", u, sizeof u);
        r->total_wh = v * unit_mult(u, 1e6);
    }
    return 1;
}

void inv_soliscloud_sign(const char * key_id, const char * key_secret, const char * body,
                         const char * date, const char * path,
                         char * content_md5, size_t md5n, char * auth, size_t authn) {
    uint8_t md5[16], mac[20];
    char md5b[32], macb[32];
    inv_md5(body, strlen(body), md5);
    inv_b64(md5, 16, md5b);
    snprintf(content_md5, md5n, "%s", md5b);
    char sts[512];
    int n = snprintf(sts, sizeof sts, "POST\n%s\napplication/json\n%s\n%s", md5b, date, path);
    if (n < 0) n = 0;
    if (n >= (int)sizeof sts) n = (int)sizeof sts - 1;
    inv_hmac_sha1(key_secret, strlen(key_secret), sts, (size_t)n, mac);
    inv_b64(mac, 20, macb);
    snprintf(auth, authn, "API %s:%s", key_id, macb);
}
