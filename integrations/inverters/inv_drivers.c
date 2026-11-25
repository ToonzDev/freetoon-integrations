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

int inv_parse_envoy_production(const char * body, inv_envoy_prod_t * p) {
    memset(p, 0, sizeof *p);
    if (!body || !mj_find(body, "production", NULL)) return 0;
    for (int i = 0; i < 8; i++) {
        char path[48], type[24] = "", mt[24] = "";
        snprintf(path, sizeof path, "production[%d]", i);
        const char * e = mj_find(body, path, NULL);
        if (!e) break;
        double w, t = 0, ac = 0;
        mj_str(e, "type", type, sizeof type);
        if (!mj_num(e, "wNow", &w)) continue;
        mj_num(e, "readingTime", &t);
        if (!strcmp(type, "eim")) {
            mj_str(e, "measurementType", mt, sizeof mt);
            mj_num(e, "activeCount", &ac);
            if (strcmp(mt, "production") || ac < 1) continue;   /* CT not installed / consumption */
            p->have_eim = 1; p->eim_w = w; p->eim_time = (long)t;
        } else if (!strcmp(type, "inverters")) {
            double wh = 0;
            mj_num(e, "whLifetime", &wh);
            p->have_inv = 1; p->inv_w = w; p->inv_time = (long)t; p->inv_wh = wh;
        }
    }
    return p->have_eim || p->have_inv;
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

/* ------------------------------------------------- SunSpec / Modbus TCP */

int inv_modbus_req(uint8_t * o, uint16_t tid, uint8_t unit, uint8_t fc, uint16_t addr, uint16_t count) {
    o[0] = (uint8_t)(tid >> 8);  o[1] = (uint8_t)tid;     /* transaction */
    o[2] = 0; o[3] = 0;                                    /* protocol 0 */
    o[4] = 0; o[5] = 6;                                    /* bytes after this */
    o[6] = unit;
    o[7] = fc;                                             /* 3 holding / 4 input registers */
    o[8] = (uint8_t)(addr >> 8); o[9] = (uint8_t)addr;
    o[10] = (uint8_t)(count >> 8); o[11] = (uint8_t)count;
    return 12;
}

int inv_modbus_resp(const uint8_t * in, int n, uint16_t tid, uint8_t fc, uint16_t count,
                    uint16_t * regs, char * err, size_t errn) {
    if (n < 9) { snprintf(err, errn, "kort Modbus-antwoord (%d bytes)", n); return 0; }
    if (((in[0] << 8) | in[1]) != tid) { snprintf(err, errn, "Modbus-antwoord hoort bij een ander verzoek"); return 0; }
    if (in[7] == (fc | 0x80)) {
        static const char * ex[] = { "", "functie niet ondersteund", "register bestaat niet",
                                     "ongeldige waarde", "apparaatfout" };
        int c = in[8];
        if (c == 10 || c == 11)                  /* gateway: nobody answers on that unit id */
            snprintf(err, errn, "Modbus-fout %d: geen apparaat op dit unit-id", c);
        else
            snprintf(err, errn, "Modbus-fout %d (%s)", c, c >= 1 && c <= 4 ? ex[c] : "onbekend");
        return 0;
    }
    if (in[7] != fc || in[8] != count * 2 || n < 9 + count * 2) {
        snprintf(err, errn, "onverwacht Modbus-antwoord");
        return 0;
    }
    for (int i = 0; i < count; i++) regs[i] = (uint16_t)((in[9 + 2 * i] << 8) | in[10 + 2 * i]);
    return 1;
}

int inv_sunspec_marker(const uint16_t * r) { return r[0] == 0x5375 && r[1] == 0x6e53; }

int inv_sunspec_inverter_model(int id) {
    return (id >= 101 && id <= 103) || (id >= 111 && id <= 113);
}

static double sf_pow(int16_t sf) {
    double v = 1;
    if (sf > 0) for (int i = 0; i < sf && i < 10; i++) v *= 10;
    else for (int i = 0; i < -sf && i < 10; i++) v /= 10;
    return v;
}

static int f32(const uint16_t * w, double * out) {
    uint32_t u = ((uint32_t)w[0] << 16) | w[1];
    float f;
    memcpy(&f, &u, sizeof f);
    if (f != f || f > 3.0e9f || f < -3.0e9f) return 0;   /* NaN = not implemented */
    *out = f;
    return 1;
}

int inv_sunspec_parse(int model_id, const uint16_t * m, int len, inv_reading_t * r, int * status) {
    r->power_w = 0; r->today_wh = -1; r->total_wh = -1;
    *status = 0;
    if (model_id >= 101 && model_id <= 103) {
        /* W 12, W_SF 13, WH 22-23 (acc32), WH_SF 24, St 36 */
        if (len < 37) return 0;
        int16_t w = (int16_t)m[12], wsf = (int16_t)m[13], whsf = (int16_t)m[24];
        uint32_t wh = ((uint32_t)m[22] << 16) | m[23];
        if (m[12] == 0x8000 || m[13] == 0x8000 || m[24] == 0x8000 || wh == 0) return 0;
        r->power_w = w * sf_pow(wsf);
        r->total_wh = wh * sf_pow(whsf);
        *status = m[36] == 0xffff ? 0 : m[36];
    } else if (model_id >= 111 && model_id <= 113) {
        /* W 20-21, WH 30-31, St 46 (floats are two registers, high word first) */
        if (len < 47) return 0;
        double w, wh;
        if (!f32(&m[20], &w) || !f32(&m[30], &wh) || wh <= 0) return 0;
        r->power_w = w;
        r->total_wh = wh;
        *status = m[46] == 0xffff ? 0 : m[46];
    } else {
        return 0;
    }
    if (r->power_w < 0) r->power_w = 0;            /* night consumption shows as a few W below 0 */
    if (*status == 2 || *status == 1 || *status == 8) r->power_w = 0;   /* sleeping / off / standby */
    return 1;
}

void inv_sunspec_string(const uint16_t * regs, int nregs, char * out, size_t n) {
    size_t o = 0;
    for (int i = 0; i < nregs && o + 2 < n; i++) {
        char c[2] = { (char)(regs[i] >> 8), (char)regs[i] };
        for (int k = 0; k < 2; k++)
            if (c[k] >= 0x20 && c[k] < 0x7f) out[o++] = c[k];
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ------------------------------------------ brand Modbus register maps */
/* Sources (community register lists, to be confirmed per brand by a tester):
 *  Huawei SUN2000: holding 32080 I32 active power W, 32106 U32 accumulated
 *    yield 0.01 kWh, 32114 U32 daily yield 0.01 kWh. SDongle port 502 unit 1
 *    (inverter's own WLAN: port 6607 unit 0); wants ~1 s after connecting.
 *  Sungrow (WiNet-S, port 502 unit 1): input 5031 U32 total active power W,
 *    5004 U32 total yield kWh, 5003 U16 daily yield 0.1 kWh; documented
 *    1-based, low word first.
 *  Growatt (Modbus RTU protocol II, via an RS485-TCP gateway): input 35 U32
 *    Pac 0.1 W, 55 U32 Eac total 0.1 kWh, 53 U32 Eac today 0.1 kWh.
 *  Solis / Ginlong (RS485 Modbus, via a gateway): input 3005 U32 active power
 *    W, 3009 U32 total energy kWh, 3015 U16 energy today 0.1 kWh; 1-based. */
static const inv_mb_profile_t MB_PROFILES[] = {
    { "huawei",        "Huawei",  3, 0, 1000,
      { 32080, MBV_I32, 1.0 },  { 32106, MBV_U32, 10.0 },  { 32114, MBV_U32, 10.0 } },
    { "sungrow",       "Sungrow", 4, 1, 0,
      { 5030, MBV_U32, 1.0 },   { 5003, MBV_U32, 1000.0 }, { 5002, MBV_U16, 100.0 } },
    { "growatt",       "Growatt", 4, 0, 0,
      { 35, MBV_U32, 0.1 },     { 55, MBV_U32, 100.0 },    { 53, MBV_U32, 100.0 } },
    { "solis_modbus",  "Solis",   4, 0, 0,
      { 3004, MBV_U32, 1.0 },   { 3008, MBV_U32, 1000.0 }, { 3014, MBV_U16, 100.0 } },
};

const inv_mb_profile_t * inv_mb_profile(const char * type) {
    for (size_t i = 0; i < sizeof MB_PROFILES / sizeof MB_PROFILES[0]; i++)
        if (!strcmp(MB_PROFILES[i].type, type)) return &MB_PROFILES[i];
    return NULL;
}

int inv_mb_value(const uint16_t * r, int kind, int lo_first, double * out) {
    if (kind == MBV_U16) {
        if (r[0] == 0xffff) return 0;
        *out = r[0];
        return 1;
    }
    uint32_t u = lo_first ? ((uint32_t)r[1] << 16) | r[0] : ((uint32_t)r[0] << 16) | r[1];
    if (kind == MBV_U32) {
        if (u == 0xffffffffu) return 0;
        *out = u;
        return 1;
    }
    if (kind == MBV_I32) {
        if (u == 0x7fffffffu || u == 0x80000000u) return 0;
        *out = (int32_t)u;
        return 1;
    }
    return 0;
}

int inv_mb_parse(const inv_mb_profile_t * p, const uint16_t * pw, const uint16_t * tot,
                 const uint16_t * day, inv_reading_t * r) {
    double v;
    r->power_w = 0; r->today_wh = -1; r->total_wh = -1;
    if (!inv_mb_value(pw, p->power.kind, p->lo_first, &v)) return 0;
    r->power_w = v * p->power.scale;
    if (!inv_mb_value(tot, p->total.kind, p->lo_first, &v) || v <= 0) return 0;
    r->total_wh = v * p->total.scale;
    if (day && p->today.kind && inv_mb_value(day, p->today.kind, p->lo_first, &v))
        r->today_wh = v * p->today.scale;
    if (r->power_w < 0) r->power_w = 0;               /* night consumption */
    /* a home inverter: above 1 MW or 100 GWh the map / word order is wrong */
    if (r->power_w > 1.0e6 || r->total_wh > 1.0e11) return 0;
    return 1;
}
