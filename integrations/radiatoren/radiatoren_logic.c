/*
 * radiatoren_logic.c - see radiatoren_logic.h. C port of RadiatorenLogic.js
 * plus the edit functions of RadiatorenApp.qml and a small JSON reader/writer.
 *
 * Only plain C99 + POSIX: builds for the Toon (glibc 2.21 at runtime) and for
 * the host unit test.
 */
#include "radiatoren_logic.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ======================================================================== */
/* Numbers, times                                                           */
/* ======================================================================== */

static double js_round(double x) { return floor(x + 0.5); }   /* Math.round */

double rad_clamp_temp(double value) {
    double n = value;
    if (isnan(n)) n = RAD_MIN_TEMP;
    n = js_round(n / RAD_STEP) * RAD_STEP;
    if (n < RAD_MIN_TEMP) n = RAD_MIN_TEMP;
    if (n > RAD_MAX_TEMP) n = RAD_MAX_TEMP;
    return n;
}

int rad_clamp_minutes(double value) {
    double n = js_round(value / 15.0) * 15.0;
    if (isnan(n) || n < 0) n = 0;
    if (n > 1440) n = 1440;
    return (int)n;
}

void rad_format_temp(double value, char * out, size_t n) {
    if (isnan(value)) { snprintf(out, n, "--"); return; }
    snprintf(out, n, "%.1f", value);
    char * dot = strchr(out, '.');
    if (dot) *dot = ',';
}

void rad_format_minutes(int minutes, char * out, size_t n) {
    int total = rad_clamp_minutes((double)minutes);
    snprintf(out, n, "%02d:%02d", total / 60, total % 60);
}

int rad_day_index(const struct tm * tm) { return (tm->tm_wday + 6) % 7; }
int rad_minutes_of_day(const struct tm * tm) { return tm->tm_hour * 60 + tm->tm_min; }

rad_block_t rad_new_block(void) {
    rad_block_t b = { { 1, 1, 1, 1, 1, 0, 0 }, 420, 1350 };
    return b;
}

void rad_normalize_block(rad_block_t * b) {
    for (int i = 0; i < 7; i++) b->days[i] = b->days[i] ? 1 : 0;
    int start = rad_clamp_minutes(b->start);
    int end = rad_clamp_minutes(b->end);
    if (start > 1425) start = 1425;
    if (end <= start) end = start + 15;
    b->start = start;
    b->end = end;
}

/* ======================================================================== */
/* Rooms and settings                                                       */
/* ======================================================================== */

/* Cut a UTF-8 string after max_chars code points (JS substring(0, 24) counts
 * UTF-16 units; for names that only differs for emoji). */
static void utf8_truncate(char * s, int max_chars) {
    int chars = 0;
    for (char * p = s; *p; ) {
        if (chars == max_chars) { *p = 0; return; }
        unsigned char c = (unsigned char)*p;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        for (int k = 0; k < len && *p; k++) p++;
        chars++;
    }
}

static void copy_str(char * dst, size_t n, const char * src) {
    snprintf(dst, n, "%s", src ? src : "");
}

void rad_default_room(rad_room_t * r, const char * id) {
    memset(r, 0, sizeof *r);
    copy_str(r->id, sizeof r->id, id && id[0] ? id : "kamer");
    copy_str(r->name, sizeof r->name, "Kamer");
    r->comfort = 20;
    r->eco = 16;
    r->mode = RAD_MODE_AUTO;
    r->manual_temp = 20;
    r->n_blocks = 0;
    r->has_measured = 0;
    r->measured = NAN;
}

static void normalize_room(rad_room_t * r) {
    if (!r->id[0]) copy_str(r->id, sizeof r->id, "kamer");
    if (!r->name[0]) copy_str(r->name, sizeof r->name, "Kamer");
    utf8_truncate(r->name, RAD_NAME_CHARS);
    r->comfort = rad_clamp_temp(r->comfort);
    r->eco = rad_clamp_temp(r->eco);
    r->manual_temp = rad_clamp_temp(r->manual_temp);
    if (r->mode != RAD_MODE_AUTO && r->mode != RAD_MODE_MANUAL && r->mode != RAD_MODE_OFF)
        r->mode = RAD_MODE_AUTO;
    if (r->n_blocks < 0) r->n_blocks = 0;
    if (r->n_blocks > RAD_MAX_BLOCKS) r->n_blocks = RAD_MAX_BLOCKS;
    for (int i = 0; i < r->n_blocks; i++) rad_normalize_block(&r->blocks[i]);
    if (r->has_measured && isnan(r->measured)) r->has_measured = 0;
}

static int id_taken(const rad_settings_t * s, int upto, const char * id) {
    for (int i = 0; i < upto; i++)
        if (strcmp(s->rooms[i].id, id) == 0) return 1;
    return 0;
}

void rad_normalize_settings(rad_settings_t * s) {
    if (s->n_rooms < 0) s->n_rooms = 0;
    if (s->n_rooms > RAD_MAX_ROOMS) s->n_rooms = RAD_MAX_ROOMS;
    s->boiler_control = s->boiler_control ? 1 : 0;
    s->hysteresis = 0.5;
    for (int i = 0; i < s->n_rooms; i++) {
        rad_room_t * r = &s->rooms[i];
        normalize_room(r);
        while (id_taken(s, i, r->id)) {
            size_t len = strlen(r->id);
            if (len + 1 >= sizeof r->id) { r->id[len - 1] = 'x'; continue; }  /* keep changing */
            r->id[len] = 'x';
            r->id[len + 1] = 0;
        }
    }
}

void rad_next_room_id(const rad_settings_t * s, char * out, size_t n) {
    long highest = 0;
    for (int i = 0; i < s->n_rooms; i++) {
        const char * id = s->rooms[i].id;
        if (strncmp(id, "kamer", 5) != 0 || !id[5]) continue;
        const char * p = id + 5;
        int digits = 1;
        for (const char * q = p; *q; q++) if (!isdigit((unsigned char)*q)) { digits = 0; break; }
        if (!digits) continue;
        long v = strtol(p, NULL, 10);
        if (v > highest) highest = v;
    }
    snprintf(out, n, "kamer%ld", highest + 1);
}

int rad_in_comfort(const rad_room_t * r, const struct tm * tm) {
    int day = rad_day_index(tm);
    int now = rad_minutes_of_day(tm);
    for (int i = 0; i < r->n_blocks; i++) {
        const rad_block_t * b = &r->blocks[i];
        if (b->days[day] && now >= b->start && now < b->end) return 1;
    }
    return 0;
}

rad_target_t rad_target_for(const rad_room_t * r, const struct tm * tm) {
    rad_target_t t;
    if (r->mode == RAD_MODE_OFF) { t.target = RAD_MIN_TEMP; t.reason = RAD_REASON_OFF; }
    else if (r->mode == RAD_MODE_MANUAL) { t.target = r->manual_temp; t.reason = RAD_REASON_MANUAL; }
    else if (rad_in_comfort(r, tm)) { t.target = r->comfort; t.reason = RAD_REASON_COMFORT; }
    else { t.target = r->eco; t.reason = RAD_REASON_ECO; }
    return t;
}

int rad_next_change_today(const rad_room_t * r, const struct tm * tm) {
    if (r->mode != RAD_MODE_AUTO) return -1;
    int day = rad_day_index(tm);
    int now = rad_minutes_of_day(tm);
    int best = -1;
    for (int i = 0; i < r->n_blocks; i++) {
        const rad_block_t * b = &r->blocks[i];
        if (!b->days[day]) continue;
        int edges[2] = { b->start, b->end };
        for (int j = 0; j < 2; j++)
            if (edges[j] > now && edges[j] < 1440 && (best < 0 || edges[j] < best)) best = edges[j];
    }
    return best;
}

int rad_room_demands_heat(const rad_room_t * r, const struct tm * tm, double hysteresis) {
    if (!r->has_measured) return 0;
    if (r->mode == RAD_MODE_OFF) return 0;
    return r->measured < rad_target_for(r, tm).target - hysteresis;
}

int rad_demanding_rooms(const rad_settings_t * s, const struct tm * tm, double hysteresis,
                        int * out_idx, int max) {
    int n = 0;
    for (int i = 0; i < s->n_rooms; i++)
        if (rad_room_demands_heat(&s->rooms[i], tm, hysteresis)) {
            if (out_idx && n < max) out_idx[n] = i;
            n++;
        }
    return n;
}

int rad_boiler_setpoint_advice(const rad_settings_t * s, const struct tm * tm,
                               double hysteresis, double toon_room_temp, double * out) {
    int measured_rooms = 0;
    double lowest_eco = RAD_MAX_TEMP;
    for (int i = 0; i < s->n_rooms; i++) {
        if (s->rooms[i].has_measured) measured_rooms++;
        if (s->rooms[i].eco < lowest_eco) lowest_eco = s->rooms[i].eco;
    }
    if (measured_rooms == 0) return 0;
    if (rad_demanding_rooms(s, tm, hysteresis, NULL, 0) == 0) { *out = rad_clamp_temp(lowest_eco); return 1; }
    if (isnan(toon_room_temp)) return 0;
    *out = rad_clamp_temp(ceil(toon_room_temp * 2) / 2 + 1.5);
    return 1;
}

const char * rad_mode_str(rad_mode_t m) {
    return m == RAD_MODE_MANUAL ? "manual" : m == RAD_MODE_OFF ? "off" : "auto";
}

const char * rad_reason_str(rad_reason_t r) {
    switch (r) {
    case RAD_REASON_COMFORT: return "comfort";
    case RAD_REASON_ECO:     return "eco";
    case RAD_REASON_MANUAL:  return "handmatig";
    default:                 return "uit";
    }
}

/* ======================================================================== */
/* Edits                                                                    */
/* ======================================================================== */

static rad_room_t * room_at(rad_settings_t * s, int idx) {
    return (idx >= 0 && idx < s->n_rooms) ? &s->rooms[idx] : NULL;
}

int rad_room_index(const rad_settings_t * s, const char * id) {
    if (!id) return -1;
    for (int i = 0; i < s->n_rooms; i++)
        if (strcmp(s->rooms[i].id, id) == 0) return i;
    return -1;
}

int rad_add_room(rad_settings_t * s, char * new_id, size_t n) {
    if (s->n_rooms >= RAD_MAX_ROOMS) return 0;
    char id[RAD_ID_BYTES];
    rad_next_room_id(s, id, sizeof id);
    rad_room_t * r = &s->rooms[s->n_rooms];
    rad_default_room(r, id);
    snprintf(r->name, sizeof r->name, "Kamer %d", s->n_rooms + 1);
    r->n_blocks = 1;
    r->blocks[0] = rad_new_block();
    s->n_rooms++;
    rad_normalize_settings(s);
    if (new_id) copy_str(new_id, n, s->rooms[s->n_rooms - 1].id);
    return 1;
}

int rad_remove_room(rad_settings_t * s, int idx) {
    if (!room_at(s, idx)) return 0;
    memmove(&s->rooms[idx], &s->rooms[idx + 1], (size_t)(s->n_rooms - idx - 1) * sizeof s->rooms[0]);
    s->n_rooms--;
    return 1;
}

int rad_rename_room(rad_settings_t * s, int idx, const char * name) {
    rad_room_t * r = room_at(s, idx);
    if (!r || !name) return 0;
    while (*name && isspace((unsigned char)*name)) name++;
    size_t len = strlen(name);
    while (len > 0 && isspace((unsigned char)name[len - 1])) len--;
    if (len == 0) return 0;
    if (len >= sizeof r->name) len = sizeof r->name - 1;
    memcpy(r->name, name, len);
    r->name[len] = 0;
    utf8_truncate(r->name, RAD_NAME_CHARS);
    return 1;
}

int rad_cycle_mode(rad_settings_t * s, int idx, const struct tm * now) {
    rad_room_t * r = room_at(s, idx);
    if (!r) return 0;
    if (r->mode == RAD_MODE_AUTO) {
        r->manual_temp = rad_target_for(r, now).target;
        r->mode = RAD_MODE_MANUAL;
    } else if (r->mode == RAD_MODE_MANUAL) {
        r->mode = RAD_MODE_OFF;
    } else {
        r->mode = RAD_MODE_AUTO;
    }
    return 1;
}

int rad_set_mode(rad_settings_t * s, int idx, rad_mode_t mode, const struct tm * now) {
    rad_room_t * r = room_at(s, idx);
    if (!r || r->mode == mode) return 0;
    /* Like cycleMode: switching from the schedule to manual starts at the
     * temperature the schedule asks for right now. */
    if (mode == RAD_MODE_MANUAL && r->mode == RAD_MODE_AUTO)
        r->manual_temp = rad_target_for(r, now).target;
    r->mode = mode;
    return 1;
}

int rad_nudge_target(rad_settings_t * s, int idx, double delta, const struct tm * now) {
    rad_room_t * r = room_at(s, idx);
    if (!r) return 0;
    /* From "off" continue from the last manual value, not from frost protection. */
    double current = r->mode == RAD_MODE_AUTO ? rad_target_for(r, now).target : r->manual_temp;
    r->manual_temp = rad_clamp_temp(current + delta);
    r->mode = RAD_MODE_MANUAL;
    return 1;
}

static int nudge_field(double * field, double delta) {
    double v = rad_clamp_temp(*field + delta);
    if (v == *field) return 0;
    *field = v;
    return 1;
}

int rad_nudge_comfort(rad_settings_t * s, int idx, double delta) {
    rad_room_t * r = room_at(s, idx);
    return r ? nudge_field(&r->comfort, delta) : 0;
}

int rad_nudge_eco(rad_settings_t * s, int idx, double delta) {
    rad_room_t * r = room_at(s, idx);
    return r ? nudge_field(&r->eco, delta) : 0;
}

int rad_add_block(rad_settings_t * s, int idx) {
    rad_room_t * r = room_at(s, idx);
    if (!r || r->n_blocks >= RAD_MAX_BLOCKS) return 0;
    r->blocks[r->n_blocks++] = rad_new_block();
    return 1;
}

int rad_remove_block(rad_settings_t * s, int idx, int block) {
    rad_room_t * r = room_at(s, idx);
    if (!r || block < 0 || block >= r->n_blocks) return 0;
    memmove(&r->blocks[block], &r->blocks[block + 1], (size_t)(r->n_blocks - block - 1) * sizeof r->blocks[0]);
    r->n_blocks--;
    return 1;
}

int rad_toggle_block_day(rad_settings_t * s, int idx, int block, int day) {
    rad_room_t * r = room_at(s, idx);
    if (!r || block < 0 || block >= r->n_blocks || day < 0 || day > 6) return 0;
    r->blocks[block].days[day] = !r->blocks[block].days[day];
    return 1;
}

int rad_nudge_block_time(rad_settings_t * s, int idx, int block, int is_end, int delta) {
    rad_room_t * r = room_at(s, idx);
    if (!r || block < 0 || block >= r->n_blocks) return 0;
    rad_block_t * b = &r->blocks[block];
    int * field = is_end ? &b->end : &b->start;
    int v = rad_clamp_minutes((double)(*field + delta));
    /* Start stays before the end; normalisation catches the rest. */
    if (!is_end && v >= b->end) return 0;
    if (is_end && v <= b->start) return 0;
    if (v == *field) return 0;
    *field = v;
    rad_normalize_block(b);
    return 1;
}

/* ======================================================================== */
/* Minimal JSON DOM                                                         */
/* ======================================================================== */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval {
    jtype_t t;
    int b;
    double num;
    char * str;
    int n;
    struct jval ** items;
    char ** keys;                     /* objects only */
} jval_t;

typedef struct { const char * p; int depth; int err; } jparser_t;

static void jfree(jval_t * v) {
    if (!v) return;
    free(v->str);
    for (int i = 0; i < v->n; i++) {
        jfree(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

static void jws(jparser_t * ps) { while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++; }

static jval_t * jnew(jtype_t t) {
    jval_t * v = calloc(1, sizeof *v);
    if (v) v->t = t;
    return v;
}

static void utf8_put(char ** o, unsigned cp) {
    char * q = *o;
    if (cp < 0x80) *q++ = (char)cp;
    else if (cp < 0x800) { *q++ = (char)(0xC0 | (cp >> 6)); *q++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *q++ = (char)(0xE0 | (cp >> 12)); *q++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *q++ = (char)(0x80 | (cp & 0x3F)); }
    else { *q++ = (char)(0xF0 | (cp >> 18)); *q++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *q++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *q++ = (char)(0x80 | (cp & 0x3F)); }
    *o = q;
}

static int hex4(const char * p, unsigned * out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char * jparse_string(jparser_t * ps) {
    if (*ps->p != '"') { ps->err = 1; return NULL; }
    ps->p++;
    const char * start = ps->p;
    size_t cap = 0;
    while (start[cap] && start[cap] != '"') { if (start[cap] == '\\' && start[cap + 1]) cap++; cap++; }
    char * out = malloc(cap * 2 + 8);              /* \u escapes never grow more than 2x */
    if (!out) { ps->err = 1; return NULL; }
    char * o = out;
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') { *o++ = c; continue; }
        char e = *ps->p++;
        switch (e) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp;
            if (!hex4(ps->p, &cp)) { ps->err = 1; free(out); return NULL; }
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            utf8_put(&o, cp);
            break;
        }
        default: ps->err = 1; free(out); return NULL;
        }
    }
    if (*ps->p != '"') { ps->err = 1; free(out); return NULL; }
    ps->p++;
    *o = 0;
    return out;
}

static jval_t * jparse_value(jparser_t * ps);

static int jpush(jval_t * v, jval_t * item, char * key) {
    jval_t ** items = realloc(v->items, (size_t)(v->n + 1) * sizeof *items);
    if (!items) return 0;
    v->items = items;
    if (v->t == J_OBJ) {
        char ** keys = realloc(v->keys, (size_t)(v->n + 1) * sizeof *keys);
        if (!keys) return 0;
        v->keys = keys;
        v->keys[v->n] = key;
    }
    v->items[v->n++] = item;
    return 1;
}

static jval_t * jparse_value(jparser_t * ps) {
    jws(ps);
    if (++ps->depth > 32) { ps->err = 1; return NULL; }
    jval_t * v = NULL;
    char c = *ps->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        v = jnew(obj ? J_OBJ : J_ARR);
        if (!v) { ps->err = 1; goto out; }
        ps->p++;
        jws(ps);
        if (*ps->p == (obj ? '}' : ']')) { ps->p++; goto out; }
        for (;;) {
            char * key = NULL;
            jws(ps);
            if (obj) {
                key = jparse_string(ps);
                if (ps->err) goto fail;
                jws(ps);
                if (*ps->p != ':') { free(key); ps->err = 1; goto fail; }
                ps->p++;
            }
            jval_t * item = jparse_value(ps);
            if (ps->err || !item || !jpush(v, item, key)) { jfree(item); free(key); ps->err = 1; goto fail; }
            jws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == (obj ? '}' : ']')) { ps->p++; break; }
            ps->err = 1;
            goto fail;
        }
    } else if (c == '"') {
        v = jnew(J_STR);
        if (!v) { ps->err = 1; goto out; }
        v->str = jparse_string(ps);
        if (ps->err) goto fail;
    } else if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
        v = jnew(J_BOOL);
        if (!v) { ps->err = 1; goto out; }
        v->b = ps->p[0] == 't';
        ps->p += v->b ? 4 : 5;
    } else if (!strncmp(ps->p, "null", 4)) {
        v = jnew(J_NULL);
        if (!v) { ps->err = 1; goto out; }
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char * end;
        double d = strtod(ps->p, &end);
        if (end == ps->p) { ps->err = 1; goto out; }
        v = jnew(J_NUM);
        if (!v) { ps->err = 1; goto out; }
        v->num = d;
        ps->p = end;
    } else {
        ps->err = 1;
    }
out:
    ps->depth--;
    return v;
fail:
    jfree(v);
    ps->depth--;
    return NULL;
}

static jval_t * jparse(const char * text) {
    if (!text) return NULL;
    jparser_t ps = { text, 0, 0 };
    jval_t * v = jparse_value(&ps);
    if (ps.err) { jfree(v); return NULL; }
    jws(&ps);
    if (*ps.p) { jfree(v); return NULL; }   /* JSON.parse rejects trailing garbage */
    return v;
}

static const jval_t * jget(const jval_t * obj, const char * key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    const jval_t * found = NULL;
    for (int i = 0; i < obj->n; i++)            /* last one wins, like JSON.parse */
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) found = obj->items[i];
    return found;
}

/* JS Number(x); missing (undefined) is NaN. */
static double js_number(const jval_t * v) {
    if (!v) return NAN;
    switch (v->t) {
    case J_NULL: return 0;
    case J_BOOL: return v->b;
    case J_NUM: return v->num;
    case J_STR: {
        const char * p = v->str;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return 0;
        char * end;
        double d = strtod(p, &end);
        if (end == p) return NAN;
        while (*end && isspace((unsigned char)*end)) end++;
        return *end ? NAN : d;
    }
    case J_ARR: return v->n == 0 ? 0 : (v->n == 1 ? js_number(v->items[0]) : NAN);
    default: return NAN;
    }
}

static void js_number_to_string(double d, char * out, size_t n) {
    if (isnan(d)) { snprintf(out, n, "NaN"); return; }
    if (d == floor(d) && fabs(d) < 1e15) { snprintf(out, n, "%.0f", d); return; }
    snprintf(out, n, "%.15g", d);
    if (strtod(out, NULL) != d) snprintf(out, n, "%.17g", d);
}

/* JS truthiness, for `source.name || "Kamer"`. */
static int js_truthy(const jval_t * v) {
    if (!v) return 0;
    switch (v->t) {
    case J_NULL: return 0;
    case J_BOOL: return v->b;
    case J_NUM: return v->num != 0 && !isnan(v->num);
    case J_STR: return v->str[0] != 0;
    default: return 1;
    }
}

/* JS String(x) for the scalar cases the settings use. */
static void js_string(const jval_t * v, char * out, size_t n) {
    if (!v) { snprintf(out, n, "undefined"); return; }
    switch (v->t) {
    case J_NULL: snprintf(out, n, "null"); break;
    case J_BOOL: snprintf(out, n, "%s", v->b ? "true" : "false"); break;
    case J_NUM: js_number_to_string(v->num, out, n); break;
    case J_STR: snprintf(out, n, "%s", v->str); break;
    case J_OBJ: snprintf(out, n, "[object Object]"); break;
    default: snprintf(out, n, "%s", ""); break;
    }
}

static double temp_or_default(const jval_t * room, const char * key, double dflt) {
    const jval_t * v = jget(room, key);
    return rad_clamp_temp(v ? js_number(v) : dflt);   /* `=== undefined ? 20 : value` */
}

static void room_from_json(rad_room_t * r, const jval_t * src, const char * fallback_id) {
    rad_default_room(r, fallback_id);
    const jval_t * v;

    v = jget(src, "id");
    if (js_truthy(v)) js_string(v, r->id, sizeof r->id);
    else copy_str(r->id, sizeof r->id, fallback_id && fallback_id[0] ? fallback_id : "kamer");

    v = jget(src, "name");
    if (js_truthy(v)) js_string(v, r->name, sizeof r->name);
    utf8_truncate(r->name, RAD_NAME_CHARS);

    v = jget(src, "valveUuid");
    if (js_truthy(v)) js_string(v, r->valve_uuid, sizeof r->valve_uuid);

    r->comfort = temp_or_default(src, "comfort", 20);
    r->eco = temp_or_default(src, "eco", 16);
    r->manual_temp = temp_or_default(src, "manualTemp", 20);

    v = jget(src, "mode");
    r->mode = RAD_MODE_AUTO;
    if (v && v->t == J_STR) {
        if (!strcmp(v->str, "manual")) r->mode = RAD_MODE_MANUAL;
        else if (!strcmp(v->str, "off")) r->mode = RAD_MODE_OFF;
    }

    v = jget(src, "measured");
    if (v && v->t != J_NULL && !isnan(js_number(v))) { r->has_measured = 1; r->measured = js_number(v); }

    v = jget(src, "blocks");
    r->n_blocks = 0;
    if (v && v->t == J_ARR) {
        for (int i = 0; i < v->n && r->n_blocks < RAD_MAX_BLOCKS; i++) {
            const jval_t * bs = v->items[i];
            rad_block_t * b = &r->blocks[r->n_blocks++];
            memset(b, 0, sizeof *b);
            const jval_t * days = jget(bs, "days");
            for (int d = 0; d < 7; d++)          /* `source.days[i] === true` */
                b->days[d] = (days && days->t == J_ARR && d < days->n
                              && days->items[d]->t == J_BOOL && days->items[d]->b) ? 1 : 0;
            b->start = rad_clamp_minutes(js_number(jget(bs, "start")));
            b->end = rad_clamp_minutes(js_number(jget(bs, "end")));
            rad_normalize_block(b);
        }
    }
}

void rad_settings_from_json(rad_settings_t * s, const char * json) {
    memset(s, 0, sizeof *s);
    s->hysteresis = 0.5;
    jval_t * root = jparse(json);
    if (root && root->t == J_OBJ) {
        const jval_t * bc = jget(root, "boilerControl");
        s->boiler_control = bc && bc->t == J_BOOL && bc->b;
        const jval_t * rooms = jget(root, "rooms");
        if (rooms && rooms->t == J_ARR) {
            for (int i = 0; i < rooms->n && s->n_rooms < RAD_MAX_ROOMS; i++) {
                char fb[24];
                snprintf(fb, sizeof fb, "kamer%d", i + 1);
                room_from_json(&s->rooms[s->n_rooms++], rooms->items[i], fb);
            }
        }
    }
    jfree(root);
    rad_normalize_settings(s);
}

/* ======================================================================== */
/* JSON writer                                                              */
/* ======================================================================== */

typedef struct { char * buf; size_t len, cap; int oom; } sbuf_t;

static void sb_put(sbuf_t * b, const char * s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (b->len + n + 1 > cap) cap *= 2;
        char * nb = realloc(b->buf, cap);
        if (!nb) { b->oom = 1; return; }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

static void sb_str(sbuf_t * b, const char * s) { sb_put(b, s, strlen(s)); }

static void sb_fmt(sbuf_t * b, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(sbuf_t * b, const char * fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void sb_num(sbuf_t * b, double d) {
    char tmp[40];
    js_number_to_string(d, tmp, sizeof tmp);
    sb_str(b, tmp);
}

/* JSON.stringify string escaping; UTF-8 passes through. */
static void sb_jstr(sbuf_t * b, const char * s) {
    sb_put(b, "\"", 1);
    for (const unsigned char * p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        default:
            if (*p < 0x20) sb_fmt(b, "\\u%04x", *p);
            else sb_put(b, (const char *)p, 1);
        }
    }
    sb_put(b, "\"", 1);
}

static void sb_indent(sbuf_t * b, int level) {
    for (int i = 0; i < level; i++) sb_str(b, "  ");
}

char * rad_settings_to_json(const rad_settings_t * s) {
    sbuf_t b = { 0 };
    sb_str(&b, "{\n  \"schema\": ");
    sb_jstr(&b, RAD_SETTINGS_SCHEMA);
    sb_str(&b, ",\n  \"boilerControl\": ");
    sb_str(&b, s->boiler_control ? "true" : "false");
    sb_str(&b, ",\n  \"hysteresis\": ");
    sb_num(&b, s->hysteresis);
    sb_str(&b, ",\n  \"rooms\": ");
    if (s->n_rooms == 0) sb_str(&b, "[]");
    else {
        sb_str(&b, "[\n");
        for (int i = 0; i < s->n_rooms; i++) {
            const rad_room_t * r = &s->rooms[i];
            sb_indent(&b, 2); sb_str(&b, "{\n");
            sb_indent(&b, 3); sb_str(&b, "\"id\": "); sb_jstr(&b, r->id); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"name\": "); sb_jstr(&b, r->name); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"valveUuid\": "); sb_jstr(&b, r->valve_uuid); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"comfort\": "); sb_num(&b, r->comfort); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"eco\": "); sb_num(&b, r->eco); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"mode\": "); sb_jstr(&b, rad_mode_str(r->mode)); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"manualTemp\": "); sb_num(&b, r->manual_temp); sb_str(&b, ",\n");
            sb_indent(&b, 3); sb_str(&b, "\"blocks\": ");
            if (r->n_blocks == 0) sb_str(&b, "[]");
            else {
                sb_str(&b, "[\n");
                for (int k = 0; k < r->n_blocks; k++) {
                    const rad_block_t * bl = &r->blocks[k];
                    sb_indent(&b, 4); sb_str(&b, "{\n");
                    sb_indent(&b, 5); sb_str(&b, "\"days\": [\n");
                    for (int d = 0; d < 7; d++) {
                        sb_indent(&b, 6);
                        sb_str(&b, bl->days[d] ? "true" : "false");
                        sb_str(&b, d < 6 ? ",\n" : "\n");
                    }
                    sb_indent(&b, 5); sb_str(&b, "],\n");
                    sb_indent(&b, 5); sb_fmt(&b, "\"start\": %d,\n", bl->start);
                    sb_indent(&b, 5); sb_fmt(&b, "\"end\": %d\n", bl->end);
                    sb_indent(&b, 4); sb_str(&b, k < r->n_blocks - 1 ? "},\n" : "}\n");
                }
                sb_indent(&b, 3); sb_str(&b, "]");
            }
            sb_str(&b, "\n");
            sb_indent(&b, 2); sb_str(&b, i < s->n_rooms - 1 ? "},\n" : "}\n");
        }
        sb_indent(&b, 1); sb_str(&b, "]");
    }
    sb_str(&b, "\n}");
    if (b.oom) { free(b.buf); return NULL; }
    return b.buf;
}

char * rad_publish_json(const rad_settings_t * s, time_t now, const struct tm * tm) {
    sbuf_t b = { 0 };
    sb_str(&b, "{\"schema\":");
    sb_jstr(&b, RAD_TARGETS_SCHEMA);
    sb_fmt(&b, ",\"updated\":%lld,\"hysteresis\":", (long long)now);
    sb_num(&b, s->hysteresis);
    sb_str(&b, ",\"rooms\":[");
    for (int i = 0; i < s->n_rooms; i++) {
        const rad_room_t * r = &s->rooms[i];
        rad_target_t t = rad_target_for(r, tm);
        if (i) sb_str(&b, ",");
        sb_str(&b, "{\"id\":"); sb_jstr(&b, r->id);
        sb_str(&b, ",\"name\":"); sb_jstr(&b, r->name);
        sb_str(&b, ",\"target\":"); sb_num(&b, t.target);
        sb_str(&b, ",\"mode\":"); sb_jstr(&b, rad_mode_str(r->mode));
        sb_str(&b, ",\"reason\":"); sb_jstr(&b, rad_reason_str(t.reason));
        sb_str(&b, "}");
    }
    sb_str(&b, "]}");
    if (b.oom) { free(b.buf); return NULL; }
    return b.buf;
}

/* ======================================================================== */
/* Files                                                                    */
/* ======================================================================== */

char * rad_read_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char * buf = malloc(cap);
    while (buf) {
        if (len + 1024 + 1 > cap) {
            if (cap >= (1u << 20)) break;            /* settings are tiny; refuse > 1 MB */
            char * nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
    }
    fclose(f);
    if (buf) buf[len] = 0;
    return buf;
}

int rad_mkdirs(const char * dir) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char * p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int rad_write_atomic(const char * path, const char * data, int durable) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t len = strlen(data), off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    if (durable) fsync(fd);
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int rad_load_settings(rad_settings_t * s, const char * path) {
    char * text = rad_read_file(path);
    rad_settings_from_json(s, text);
    int existed = text != NULL;
    free(text);
    return existed;
}

int rad_save_settings(const rad_settings_t * s, const char * path) {
    rad_settings_t copy = *s;
    rad_normalize_settings(&copy);
    char * json = rad_settings_to_json(&copy);
    if (!json) return -1;
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char * slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; rad_mkdirs(dir); }
    int rc = rad_write_atomic(path, json, 1);
    free(json);
    return rc;
}
