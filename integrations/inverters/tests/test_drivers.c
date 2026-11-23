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

int main(void) {
    test_crypto();
    test_soliscloud_sign();
    test_minijson();
    test_envoy();
    test_stick();
    test_soliscloud();
    printf("%d/%d checks passed\n", runs - fails, runs);
    return fails != 0;
}

const char * expected_solis_auth = "API 1300386381676000000:zDXDr5XlLjDbiaQZXE0DWSI/33A=";
