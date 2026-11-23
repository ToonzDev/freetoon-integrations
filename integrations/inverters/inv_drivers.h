#ifndef INV_DRIVERS_H
#define INV_DRIVERS_H
#include <stddef.h>

/* Response parsers and request builders for each inverter, free of I/O so
 * tests/test_drivers.c can run them on the build host against synthetic
 * fixtures. Driver logic ported from oepi-loepi's solarpanel-plugins
 * (ENPHASE1 / ENPHASE-T1, OMNIK2, SOLISCLOUD1), with his permission. */

typedef struct {
    double power_w;     /* production now, W */
    double today_wh;    /* today, Wh; < 0 = not reported */
    double total_wh;    /* lifetime, Wh; < 0 = not reported */
} inv_reading_t;

/* Enphase Envoy /api/v1/production:
 * {"wattHoursToday":..,"wattHoursSevenDays":..,"wattHoursLifetime":..,"wattsNow":..} */
int inv_parse_envoy_v1(const char * body, inv_reading_t * r);
/* <sn>...</sn> from the Envoy's /info.xml. 1 = found. */
int inv_parse_envoy_serial(const char * body, char * out, size_t n);

/* WiFi data logger of Solis (Ginlong) and newer Omnik inverters, /status.html:
 *   var webdata_now_p = "345"; var webdata_today_e = "1.30"; var webdata_total_e = "6543.2";
 * (W, kWh, kWh). An empty now_p with a total means the inverter sleeps: 0 W. */
int inv_parse_stick_html(const char * body, inv_reading_t * r);
/* Older Omnik logger, /js/status.js: webData="SN,model,fw,fw2,rated,W,today/100 kWh,total/10 kWh,..."
 * or myDeviceArray[0]="..." with the same fields. */
int inv_parse_stick_js(const char * body, inv_reading_t * r);

/* SolisCloud /v1/api/userStationList answer: first station's power, dayEnergy,
 * allEnergy, each scaled by its *Str unit (kW, kWh, MWh, ...). On an error
 * answer (success false / code != "0") returns 0 and copies msg into err. */
int inv_parse_soliscloud(const char * body, inv_reading_t * r, char * err, size_t errn);
/* SolisCloud request signature for a POST of `body` to `path` at `date`
 * (RFC 1123, GMT). Fills Content-MD5 and the Authorization header value
 * ("API <keyId>:<base64 hmac-sha1>"). */
void inv_soliscloud_sign(const char * key_id, const char * key_secret, const char * body,
                         const char * date, const char * path,
                         char * content_md5, size_t md5n, char * auth, size_t authn);

#endif
