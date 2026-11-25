/* Host tests for the inverter parsers and the SolisCloud signature.
 * All fixtures are synthetic (made up serials and values). Run: make test */
#include "inv_drivers.h"
#include "inv_crypto.h"
#include "minijson.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 0.001)

static void hex(const uint8_t * d, int n, char * out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_crypto(void) {
    uint8_t h[20];
    char s[64];
    inv_md5("", 0, h); hex(h, 16, s);
    CHECK(!strcmp(s, "d41d8cd98f00b204e9800998ecf8427e"));
    inv_md5("abc", 3, h); hex(h, 16, s);
    CHECK(!strcmp(s, "900150983cd24fb0d6963f7d28e17f72"));
    const char * l80 = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    inv_md5(l80, strlen(l80), h); hex(h, 16, s);
    CHECK(!strcmp(s, "57edf4a22be3c955ac49da2e2107b67a"));
    inv_sha1("abc", 3, h); hex(h, 20, s);
    CHECK(!strcmp(s, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    const char * l56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    inv_sha1(l56, strlen(l56), h); hex(h, 20, s);
    CHECK(!strcmp(s, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    const char * msg = "what do ya want for nothing?";            /* RFC 2202 case 2 */
    inv_hmac_sha1("Jefe", 4, msg, strlen(msg), h); hex(h, 20, s);
    CHECK(!strcmp(s, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    uint8_t k80[80]; memset(k80, 0xaa, sizeof k80);             /* RFC 2202 case 6: key > block */
    const char * m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    inv_hmac_sha1(k80, 80, m6, strlen(m6), h); hex(h, 20, s);
    CHECK(!strcmp(s, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
    char b[16];
    inv_b64((const uint8_t *)"f", 1, b);   CHECK(!strcmp(b, "Zg=="));
    inv_b64((const uint8_t *)"fo", 2, b);  CHECK(!strcmp(b, "Zm8="));
    inv_b64((const uint8_t *)"foo", 3, b); CHECK(!strcmp(b, "Zm9v"));
}

static void test_soliscloud_sign(void) {
    char md5[40], auth[200];
    /* Content-MD5 of this body is hard-coded in oepi-loepi's solarPanel.sh */
    inv_soliscloud_sign("1300386381676000000", "0123456789abcdef0123456789abcdef",
                        "{\"pageNo\":1,\"pageSize\":10}", "Sun, 23 Nov 2026 10:00:00 GMT",
                        "/v1/api/userStationList", md5, sizeof md5, auth, sizeof auth);
    CHECK(!strcmp(md5, "kxdxk7rbAsrzSIWgEwhH4w=="));
    /* expected HMAC computed independently:
     * printf 'POST\nkxdxk7rbAsrzSIWgEwhH4w==\napplication/json\nSun, 23 Nov 2026 10:00:00 GMT\n/v1/api/userStationList'
     *   | openssl dgst -sha1 -hmac 0123456789abcdef0123456789abcdef -binary | base64 */
    extern const char * expected_solis_auth;
    CHECK(!strcmp(auth, expected_solis_auth));
    if (strcmp(auth, expected_solis_auth)) printf("  got %s\n", auth);
}

static void test_minijson(void) {
    const char * j = " {\"a\":{\"b\":[1,{\"c\":\"x\\\"y\"},[2,3]],\"e\":\"\\u00e9\"},\"n\":-1.5e2,\"t\":true,\"s\":\"12.5\"} ";
    char s[32];
    double v;
    CHECK(mj_str(j, "a.b[1].c", s, sizeof s) && !strcmp(s, "x\"y"));
    CHECK(mj_num(j, "a.b[2][1]", &v) && NEAR(v, 3));
    CHECK(mj_num(j, "n", &v) && NEAR(v, -150));
    CHECK(mj_num(j, "s", &v) && NEAR(v, 12.5));
    CHECK(mj_bool(j, "t"));
    CHECK(mj_str(j, "a.e", s, sizeof s) && !strcmp(s, "\xc3\xa9"));
    CHECK(!mj_find(j, "a.b[3]", NULL));
    CHECK(!mj_find(j, "a.x", NULL));
    CHECK(!mj_find(j, "n.x", NULL));
    CHECK(!mj_find("{\"a\":[1,\"2", "a[1]", NULL));    /* truncated inside the value */
    const char * sub = mj_find(j, "a.b[1]", NULL);
    CHECK(sub && mj_str(sub, "c", s, sizeof s));       /* nested root */
}

static void test_envoy(void) {
    inv_reading_t r;
    CHECK(inv_parse_envoy_v1("{\"wattHoursToday\":21674,\"wattHoursSevenDays\":72437,"
                             "\"wattHoursLifetime\":4111344,\"wattsNow\":1024}", &r));
    CHECK(NEAR(r.power_w, 1024) && NEAR(r.today_wh, 21674) && NEAR(r.total_wh, 4111344));
    CHECK(inv_parse_envoy_v1("{\"wattsNow\":-3}", &r) && NEAR(r.power_w, 0) && r.today_wh < 0);
    CHECK(!inv_parse_envoy_v1("<html>401</html>", &r));
    char sn[32];
    CHECK(inv_parse_envoy_serial("<envoy_info><device><sn>122100000001</sn><pn>800-00654-r08</pn>", sn, sizeof sn)
          && !strcmp(sn, "122100000001"));
    CHECK(!inv_parse_envoy_serial("<sn>12;rm</sn>", sn, sizeof sn));
}

static void test_stick(void) {
    inv_reading_t r;
    const char * html =
        "<script type=\"text/javascript\">\n"
        "var webdata_sn = \"1234567890ABCDE\";\nvar webdata_msvn = \"\";\n"
        "var webdata_now_p = \"345\";\nvar webdata_today_e = \"1.30\";\n"
        "var webdata_total_e = \"6543.2\";\nvar webdata_alarm = \"\";\n</script>";
    CHECK(inv_parse_stick_html(html, &r));
    CHECK(NEAR(r.power_w, 345) && NEAR(r.today_wh, 1300) && NEAR(r.total_wh, 6543200));
    const char * asleep = "var webdata_now_p = \"\";\nvar webdata_today_e = \"\";\nvar webdata_total_e = \"6543.2\";";
    CHECK(inv_parse_stick_html(asleep, &r) && NEAR(r.power_w, 0) && NEAR(r.total_wh, 6543200) && r.today_wh < 0);
    CHECK(!inv_parse_stick_html("var webdata_now_p = \"\";", &r));
    CHECK(!inv_parse_stick_html("<html>login</html>", &r));

    const char * js = "var version=\"H4.01.38Y1.0.09W1.0.08\";var m2mMid=\"601234567\";\n"
                      "var webData=\"NLDN2020123456,omnik3000tl ,V5.04Build245,V4.13Build253,3000,1580,1234,45678,,0,\";";
    CHECK(inv_parse_stick_js(js, &r));
    CHECK(NEAR(r.power_w, 1580) && NEAR(r.today_wh, 12340) && NEAR(r.total_wh, 4567800));
    const char * js2 = "myDeviceArray[0]=\"NLDN2020123456,omnik2000tl,V1,V2,2000,0,15,700,,0,\";";
    CHECK(inv_parse_stick_js(js2, &r) && NEAR(r.power_w, 0) && NEAR(r.today_wh, 150) && NEAR(r.total_wh, 70000));
    CHECK(!inv_parse_stick_js("var webData=\"x,y\";", &r));
}

static void test_soliscloud(void) {
    inv_reading_t r;
    char err[80];
    const char * ok =
        "{\"success\":true,\"code\":\"0\",\"msg\":\"success\",\"data\":{\"page\":{\"current\":1,\"records\":["
        "{\"id\":\"1298491919448000000\",\"stationName\":\"Thuis\",\"power\":2.35,\"powerStr\":\"kW\","
        "\"dayEnergy\":11.4,\"dayEnergyStr\":\"kWh\",\"allEnergy\":12.75,\"allEnergyStr\":\"MWh\"}],\"total\":1}}}";
    CHECK(inv_parse_soliscloud(ok, &r, err, sizeof err));
    CHECK(NEAR(r.power_w, 2350) && NEAR(r.today_wh, 11400) && NEAR(r.total_wh, 12750000));
    const char * w = "{\"success\":true,\"code\":\"0\",\"data\":{\"page\":{\"records\":[{\"power\":512,\"powerStr\":\"W\","
                     "\"dayEnergy\":900,\"dayEnergyStr\":\"Wh\",\"allEnergy\":3456.7,\"allEnergyStr\":\"kWh\"}]}}}";
    CHECK(inv_parse_soliscloud(w, &r, err, sizeof err) && NEAR(r.power_w, 512) && NEAR(r.today_wh, 900) && NEAR(r.total_wh, 3456700));
    CHECK(!inv_parse_soliscloud("{\"success\":false,\"code\":\"Z0001\",\"msg\":\"signature error\"}", &r, err, sizeof err));
    CHECK(!strcmp(err, "signature error"));
    CHECK(!inv_parse_soliscloud("{\"success\":true,\"code\":\"0\",\"data\":{\"page\":{\"records\":[]}}}", &r, err, sizeof err));
}

/* SunSpec: synthetic register blocks laid out as in the SunSpec models
 * (SolarEdge-style 103 with scale factors, Fronius-style 113 floats). */
static void put_f32(uint16_t * w, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    w[0] = (uint16_t)(u >> 16);
    w[1] = (uint16_t)u;
}
static void test_sunspec(void) {
    uint16_t m[60];
    inv_reading_t r;
    int st;

    memset(m, 0, sizeof m);                       /* model 103, L = 50 */
    m[12] = 1234; m[13] = 0;                      /* W, W_SF */
    m[22] = (uint16_t)(15234567u >> 16); m[23] = (uint16_t)(15234567u & 0xffff); m[24] = 0;   /* WH, WH_SF */
    m[36] = 4;                                    /* St: producing */
    CHECK(inv_sunspec_parse(103, m, 50, &r, &st) && NEAR(r.power_w, 1234) && NEAR(r.total_wh, 15234567)
          && r.today_wh < 0 && st == 4);
    m[12] = 12345; m[13] = (uint16_t)-1;          /* 1234.5 W */
    m[24] = 3; m[22] = 0; m[23] = 15234;          /* kWh counter with SF 3 */
    CHECK(inv_sunspec_parse(103, m, 50, &r, &st) && NEAR(r.power_w, 1234.5) && NEAR(r.total_wh, 15234000));
    m[36] = 2; m[12] = 7; m[13] = 0;              /* sleeping: a few W reported -> 0 */
    CHECK(inv_sunspec_parse(103, m, 50, &r, &st) && NEAR(r.power_w, 0) && st == 2);
    m[36] = 4; m[12] = (uint16_t)-5;              /* night consumption -> 0 */
    CHECK(inv_sunspec_parse(103, m, 50, &r, &st) && NEAR(r.power_w, 0));
    m[12] = 0x8000;                               /* W not implemented */
    CHECK(!inv_sunspec_parse(103, m, 50, &r, &st));
    m[12] = 100; m[22] = 0; m[23] = 0;            /* WH 0: no counter */
    CHECK(!inv_sunspec_parse(103, m, 50, &r, &st));
    CHECK(!inv_sunspec_parse(103, m, 20, &r, &st));   /* block too short */
    CHECK(!inv_sunspec_parse(160, m, 50, &r, &st));   /* not an inverter model */

    memset(m, 0, sizeof m);                       /* model 113, L = 60, floats */
    put_f32(&m[20], 2500.5f);
    put_f32(&m[30], 1.0e6f);
    m[46] = 4;
    CHECK(inv_sunspec_parse(113, m, 60, &r, &st) && NEAR(r.power_w, 2500.5) && NEAR(r.total_wh, 1.0e6));
    m[20] = 0x7fc0; m[21] = 0;                    /* NaN = not implemented */
    CHECK(!inv_sunspec_parse(113, m, 60, &r, &st));

    CHECK(inv_sunspec_inverter_model(101) && inv_sunspec_inverter_model(113) && !inv_sunspec_inverter_model(1)
          && !inv_sunspec_inverter_model(104));
    uint16_t mk[2] = { 0x5375, 0x6e53 }, bad[2] = { 0x5375, 0 };
    CHECK(inv_sunspec_marker(mk) && !inv_sunspec_marker(bad));
    uint16_t sreg[8] = { ('S' << 8) | 'o', ('l' << 8) | 'a', ('r' << 8) | 'E', ('d' << 8) | 'g', ('e' << 8) | ' ', 0, 0, 0 };
    char mfr[32];
    inv_sunspec_string(sreg, 8, mfr, sizeof mfr);
    CHECK(!strcmp(mfr, "SolarEdge"));

    /* Modbus framing */
    uint8_t q[12];
    CHECK(inv_modbus_req(q, 0x0102, 1, 40069, 52) == 12);
    CHECK(q[0] == 1 && q[1] == 2 && q[5] == 6 && q[6] == 1 && q[7] == 3 && ((q[8] << 8) | q[9]) == 40069 && q[11] == 52);
    uint8_t a[13] = { 1, 2, 0, 0, 0, 7, 1, 3, 4, 0x53, 0x75, 0x6e, 0x53 };
    uint16_t regs[2];
    char err[96];
    CHECK(inv_modbus_resp(a, 13, 0x0102, 2, regs, err, sizeof err) && regs[0] == 0x5375 && regs[1] == 0x6e53);
    CHECK(!inv_modbus_resp(a, 13, 0x0103, 2, regs, err, sizeof err));          /* other transaction */
    uint8_t x[9] = { 1, 2, 0, 0, 0, 3, 1, 0x83, 2 };
    CHECK(!inv_modbus_resp(x, 9, 0x0102, 2, regs, err, sizeof err) && strstr(err, "register bestaat niet"));
    CHECK(!inv_modbus_resp(a, 11, 0x0102, 2, regs, err, sizeof err));          /* short */
}

int main(void) {
    test_crypto();
    test_soliscloud_sign();
    test_minijson();
    test_envoy();
    test_stick();
    test_soliscloud();
    test_sunspec();
    printf("%d/%d checks passed\n", runs - fails, runs);
    return fails != 0;
}

const char * expected_solis_auth = "API 1300386381676000000:zDXDr5XlLjDbiaQZXE0DWSI/33A=";
