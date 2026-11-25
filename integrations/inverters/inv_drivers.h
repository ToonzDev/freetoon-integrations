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

/* ---- SunSpec over Modbus TCP (SolarEdge, SMA, Fronius, Kostal, ...) ----
 * Registers are 16-bit words as read (host order). */
#include <stdint.h>

/* Modbus TCP read request, fc 3 (holding) or 4 (input registers): 12 bytes. */
int inv_modbus_req(uint8_t * out, uint16_t tid, uint8_t unit, uint8_t fc, uint16_t addr, uint16_t count);
/* Parse the answer to that request into regs[count]. 1 = ok; 0 with a reason
 * in err (Modbus exception, wrong transaction, short frame). */
int inv_modbus_resp(const uint8_t * in, int n, uint16_t tid, uint8_t fc, uint16_t count,
                    uint16_t * regs, char * err, size_t errn);

/* Brand-specific Modbus register maps (not SunSpec). Addresses are the
 * on-the-wire (0-based) addresses; the brands' documents often number
 * registers from 1 (Sungrow 5031 -> 5030, Solis 3005 -> 3004). */
enum { MBV_NONE = 0, MBV_U16, MBV_U32, MBV_I32 };
typedef struct {
    uint16_t addr;
    uint8_t  kind;          /* MBV_* */
    double   scale;         /* raw x scale = W (power) or Wh (energy) */
} inv_mb_field_t;
typedef struct {
    const char *   type;    /* inverters.conf TYPE */
    const char *   brand;   /* for the core's source line */
    uint8_t        fc;      /* 3 holding, 4 input */
    uint8_t        lo_first;/* 32-bit values with the low word first */
    int            connect_delay_ms;
    inv_mb_field_t power, total, today;
} inv_mb_profile_t;
/* Profile for a TYPE, or NULL. */
const inv_mb_profile_t * inv_mb_profile(const char * type);
/* One value from its registers; 0 when it is a "no value" pattern. */
int inv_mb_value(const uint16_t * regs, int kind, int lo_first, double * out);
/* Fill r from the three fields' registers (today may be NULL). 0 = power or
 * total missing / implausible. */
int inv_mb_parse(const inv_mb_profile_t * p, const uint16_t * power, const uint16_t * total,
                 const uint16_t * today, inv_reading_t * r);

/* "SunS" marker (0x5375 0x6e53) in two registers. */
int inv_sunspec_marker(const uint16_t * r);
/* Inverter models: 101-103 (integer + scale factors), 111-113 (float). */
int inv_sunspec_inverter_model(int id);
/* Readings from an inverter model block (the registers after its ID and L
 * words, `len` of them). Power from W, lifetime from WH; today is not in
 * SunSpec (-1). *status = SunSpec St (1 off, 2 sleeping, 3 starting,
 * 4 producing, 5 throttled, 6 shutting down, 7 fault, 8 standby; 0 unknown).
 * Returns 0 when W or WH is "not implemented" / invalid. */
int inv_sunspec_parse(int model_id, const uint16_t * m, int len, inv_reading_t * r, int * status);
/* ASCII string packed two characters per register (e.g. the common model's
 * manufacturer), NULs and trailing spaces dropped. */
void inv_sunspec_string(const uint16_t * regs, int nregs, char * out, size_t n);

#endif
