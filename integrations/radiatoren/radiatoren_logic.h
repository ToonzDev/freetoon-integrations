/*
 * radiatoren_logic.h - room schedule logic for the Radiatoren FreeToon app.
 *
 * C port of ../RadiatorenLogic.js (the QML app, 0.2.0). No LVGL, no FreeToon
 * headers: shared by the daemon (radiatoren_daemon.c), the UI plugin
 * (screen_radiatoren.c) and the host unit test (tests/test_logic.c).
 *
 * Settings file format is the QML app's ("nl.toon.radiatoren/settings-v1"),
 * so /mnt/data/tsc/radiatoren.userSettings.json keeps working unchanged.
 * The published file keeps the "nl.toon.radiatoren/targets-v1" contract that
 * Home Assistant reads (example package: homeassistant/radiatoren.yaml).
 */
#ifndef RADIATOREN_LOGIC_H
#define RADIATOREN_LOGIC_H

#include <stddef.h>
#include <time.h>

#define RAD_MIN_TEMP      6.0
#define RAD_MAX_TEMP      28.0
#define RAD_STEP          0.5
#define RAD_MAX_BLOCKS    4
#define RAD_MAX_ROOMS     16
#define RAD_NAME_CHARS    24          /* max characters (code points) in a name */
#define RAD_NAME_BYTES    (RAD_NAME_CHARS * 4 + 1)
#define RAD_ID_BYTES      48
#define RAD_UUID_BYTES    64

#define RAD_SETTINGS_PATH "/mnt/data/tsc/radiatoren.userSettings.json"
#define RAD_PUBLISH_PATH  "/qmf/www/tsc/radiatoren.json"
#define RAD_APP_DIR       "/mnt/data/integrations/radiatoren"

#define RAD_SETTINGS_SCHEMA "nl.toon.radiatoren/settings-v1"
#define RAD_TARGETS_SCHEMA  "nl.toon.radiatoren/targets-v1"

typedef enum { RAD_MODE_AUTO = 0, RAD_MODE_MANUAL = 1, RAD_MODE_OFF = 2 } rad_mode_t;

/* Why a room has the target it has. String values match the JS app. */
typedef enum { RAD_REASON_COMFORT = 0, RAD_REASON_ECO, RAD_REASON_MANUAL, RAD_REASON_OFF } rad_reason_t;

typedef struct {
    unsigned char days[7];            /* Monday = 0 ... Sunday = 6 */
    int start;                        /* minutes from midnight, multiple of 15 */
    int end;                          /* exclusive, > start, <= 1440 */
} rad_block_t;

typedef struct {
    char id[RAD_ID_BYTES];
    char name[RAD_NAME_BYTES];
    char valve_uuid[RAD_UUID_BYTES];
    double comfort;
    double eco;
    rad_mode_t mode;
    double manual_temp;
    int n_blocks;
    rad_block_t blocks[RAD_MAX_BLOCKS];
    int has_measured;                 /* never stored; kept for parity with the JS logic */
    double measured;
} rad_room_t;

typedef struct {
    int boiler_control;
    double hysteresis;
    int n_rooms;
    rad_room_t rooms[RAD_MAX_ROOMS];
} rad_settings_t;

typedef struct {
    double target;
    rad_reason_t reason;
} rad_target_t;

/* ---- the JS functions -------------------------------------------------- */
double rad_clamp_temp(double value);              /* NaN -> MIN_TEMP */
int    rad_clamp_minutes(double value);           /* NaN -> 0 */
void   rad_format_temp(double value, char * out, size_t n);   /* "20,5"; NaN -> "--" */
void   rad_format_minutes(int minutes, char * out, size_t n); /* "07:00" */
int    rad_day_index(const struct tm * tm);       /* Monday = 0 */
int    rad_minutes_of_day(const struct tm * tm);
rad_block_t rad_new_block(void);                  /* ma-vr 07:00-22:30 */
void   rad_normalize_block(rad_block_t * b);      /* repairs start/end */
void   rad_default_room(rad_room_t * r, const char * id);
void   rad_normalize_settings(rad_settings_t * s);   /* unique ids, clamped values */
void   rad_next_room_id(const rad_settings_t * s, char * out, size_t n);
int    rad_in_comfort(const rad_room_t * r, const struct tm * tm);
rad_target_t rad_target_for(const rad_room_t * r, const struct tm * tm);
int    rad_next_change_today(const rad_room_t * r, const struct tm * tm);  /* -1 if none */
int    rad_room_demands_heat(const rad_room_t * r, const struct tm * tm, double hysteresis);
int    rad_demanding_rooms(const rad_settings_t * s, const struct tm * tm, double hysteresis,
                           int * out_idx, int max);
/* Returns 1 and sets *out when there is advice; 0 for JS null.
 * toon_room_temp may be NaN for "unknown". */
int    rad_boiler_setpoint_advice(const rad_settings_t * s, const struct tm * tm,
                                  double hysteresis, double toon_room_temp, double * out);

const char * rad_mode_str(rad_mode_t m);          /* "auto" / "manual" / "off" */
const char * rad_reason_str(rad_reason_t r);      /* "comfort" / "eco" / "handmatig" / "uit" */

/* ---- edits (RadiatorenApp.qml). Each returns 1 when something changed. --- */
int rad_add_room(rad_settings_t * s, char * new_id, size_t n);   /* "Kamer N", one default block */
int rad_remove_room(rad_settings_t * s, int idx);
int rad_rename_room(rad_settings_t * s, int idx, const char * name);  /* trims; empty = no-op */
int rad_cycle_mode(rad_settings_t * s, int idx, const struct tm * now);
int rad_set_mode(rad_settings_t * s, int idx, rad_mode_t mode, const struct tm * now);
int rad_nudge_target(rad_settings_t * s, int idx, double delta, const struct tm * now);
int rad_nudge_comfort(rad_settings_t * s, int idx, double delta);
int rad_nudge_eco(rad_settings_t * s, int idx, double delta);
int rad_add_block(rad_settings_t * s, int idx);
int rad_remove_block(rad_settings_t * s, int idx, int block);
int rad_toggle_block_day(rad_settings_t * s, int idx, int block, int day);
int rad_nudge_block_time(rad_settings_t * s, int idx, int block, int is_end, int delta);
int rad_room_index(const rad_settings_t * s, const char * id);   /* -1 if missing */

/* ---- JSON ---------------------------------------------------------------- */
/* Parse the settings file text (QML app format); anything broken yields
 * valid, possibly empty settings, exactly like normalizeSettings(). */
void rad_settings_from_json(rad_settings_t * s, const char * json);
/* Serialise like JSON.stringify(stored, null, 2) in the QML app (no
 * "measured"). Returns a malloc()ed string. */
char * rad_settings_to_json(const rad_settings_t * s);
/* publishPayload(): {"schema":..,"updated":..,"hysteresis":..,"rooms":[..]}.
 * Returns a malloc()ed string. */
char * rad_publish_json(const rad_settings_t * s, time_t now, const struct tm * tm);

/* ---- files --------------------------------------------------------------- */
char * rad_read_file(const char * path);                         /* malloc()ed or NULL */
/* Write tmp file next to path, then rename(): readers never see half a file.
 * durable = fsync before the rename (settings; not for the 30 s publish). */
int    rad_write_atomic(const char * path, const char * data, int durable);
int    rad_load_settings(rad_settings_t * s, const char * path);  /* 1 if the file existed */
int    rad_save_settings(const rad_settings_t * s, const char * path);
int    rad_mkdirs(const char * dir);                              /* mkdir -p */

#endif
