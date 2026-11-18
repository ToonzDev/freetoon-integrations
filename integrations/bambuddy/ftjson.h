/*
 * ftjson.h - tiny JSON reader for FreeToon integrations (header-only).
 *
 * Parses a complete JSON text into a tree of jv nodes (malloc'd). Strings are
 * unescaped to UTF-8 (\uXXXX incl. surrogate pairs). Good enough for API
 * responses of a few hundred KB; not a validator.
 *
 *   jv * root = jparse(text);              NULL on a syntax error
 *   const char * s = jstr(jpath(root, "payload.departures"), "", "?");
 *   for (jv * d = jfirst(arr); d; d = d->next) ...
 *   jfree(root);
 *
 * The same file is copied into every integration folder that needs it so each
 * folder stays self-contained (one tarball, one PR).
 */
#ifndef FTJSON_H
#define FTJSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { JV_NULL = 0, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ };

typedef struct jv {
    int          type;
    char *       key;      /* member name when the parent is an object */
    char *       s;        /* JV_STR */
    double       n;        /* JV_NUM, JV_BOOL (0/1) */
    struct jv *  child;    /* JV_ARR / JV_OBJ: first element */
    struct jv *  next;     /* next sibling */
} jv;

typedef struct { const char * p; int depth; } jparser_;

static void jfree(jv * v) {
    while (v) {
        jv * nx = v->next;
        jfree(v->child);
        free(v->key);
        free(v->s);
        free(v);
        v = nx;
    }
}

static void jskip_ws_(jparser_ * ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static int jhex4_(const char * p, unsigned * out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Characters the Toon's fonts lack, as their plain look-alike (0 = keep). */
static unsigned jplain_(unsigned cp) {
    switch (cp) {
    case 0x00a0: case 0x2007: case 0x202f: case 0x2009: case 0x200a: return ' ';
    case 0x2018: case 0x2019: case 0x201b: return '\'';
    case 0x201c: case 0x201d: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    default: return 0;
    }
}

static size_t jput_utf8_(char * o, unsigned cp) {
    unsigned plain = jplain_(cp);
    if (plain) cp = plain;
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f)); return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f)); return 4;
}

/* ps->p at the opening quote. Returns a malloc'd unescaped copy. */
static char * jparse_str_(jparser_ * ps) {
    const char * p = ps->p + 1;
    size_t cap = 0;
    const char * q = p;
    while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; cap++; }
    if (*q != '"') return NULL;
    char * out = (char *)malloc(cap * 4 + 1);   /* \uXXXX can grow to 4 bytes; generous */
    if (!out) return NULL;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p != '\\') {
            /* raw UTF-8 for U+00A0 and U+2010..U+2026: map to the plain look-alike */
            const unsigned char * u = (const unsigned char *)p;
            if (u[0] == 0xc2 && u[1] == 0xa0) { out[n++] = ' '; p += 2; continue; }
            if (u[0] == 0xe2 && (u[1] == 0x80) && u[2] >= 0x80 && u[2] <= 0xbf) {
                unsigned cp = 0x2000 | (u[2] & 0x3f);
                unsigned plain = jplain_(cp);
                if (plain) { out[n++] = (char)plain; p += 3; continue; }
            }
            out[n++] = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': out[n++] = '\n'; p++; break;
        case 't': out[n++] = '\t'; p++; break;
        case 'r': out[n++] = '\r'; p++; break;
        case 'b': out[n++] = '\b'; p++; break;
        case 'f': out[n++] = '\f'; p++; break;
        case 'u': {
            unsigned cp;
            if (jhex4_(p + 1, &cp) != 0) { free(out); return NULL; }
            p += 5;
            if (cp >= 0xd800 && cp <= 0xdbff && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (jhex4_(p + 2, &lo) == 0 && lo >= 0xdc00 && lo <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    p += 6;
                }
            }
            n += jput_utf8_(out + n, cp);
            break;
        }
        default: out[n++] = *p++; break;          /* \" \\ \/ */
        }
    }
    out[n] = 0;
    ps->p = p + 1;
    return out;
}

static jv * jparse_val_(jparser_ * ps);

static jv * jnew_(int type) {
    jv * v = (jv *)calloc(1, sizeof *v);
    if (v) v->type = type;
    return v;
}

static jv * jparse_val_(jparser_ * ps) {
    jskip_ws_(ps);
    if (++ps->depth > 64) return NULL;
    jv * v = NULL;
    char c = *ps->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        v = jnew_(obj ? JV_OBJ : JV_ARR);
        if (!v) return NULL;
        ps->p++;
        jv ** tail = &v->child;
        jskip_ws_(ps);
        if (*ps->p == (obj ? '}' : ']')) { ps->p++; ps->depth--; return v; }
        for (;;) {
            char * key = NULL;
            if (obj) {
                jskip_ws_(ps);
                if (*ps->p != '"' || !(key = jparse_str_(ps))) { jfree(v); return NULL; }
                jskip_ws_(ps);
                if (*ps->p != ':') { free(key); jfree(v); return NULL; }
                ps->p++;
            }
            jv * item = jparse_val_(ps);
            if (!item) { free(key); jfree(v); return NULL; }
            item->key = key;
            *tail = item;
            tail = &item->next;
            jskip_ws_(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == (obj ? '}' : ']')) { ps->p++; break; }
            jfree(v);
            return NULL;
        }
    } else if (c == '"') {
        v = jnew_(JV_STR);
        if (!v || !(v->s = jparse_str_(ps))) { free(v); return NULL; }
    } else if (!strncmp(ps->p, "true", 4)) { v = jnew_(JV_BOOL); if (v) v->n = 1; ps->p += 4; }
    else if (!strncmp(ps->p, "false", 5)) { v = jnew_(JV_BOOL); ps->p += 5; }
    else if (!strncmp(ps->p, "null", 4)) { v = jnew_(JV_NULL); ps->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char * end;
        double d = strtod(ps->p, &end);
        if (end == ps->p) return NULL;
        v = jnew_(JV_NUM);
        if (v) v->n = d;
        ps->p = end;
    }
    ps->depth--;
    return v;
}

static jv * jparse(const char * text) {
    if (!text) return NULL;
    jparser_ ps = { text, 0 };
    return jparse_val_(&ps);
}

/* Member of an object (NULL if absent or v is not an object). */
static jv * jget(const jv * v, const char * key) {
    if (!v || v->type != JV_OBJ) return NULL;
    for (jv * c = v->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

/* Dotted path: "payload.departures", "temperatures.bed", "items.0.name". */
static jv * jpath(const jv * v, const char * path) {
    char seg[64];
    while (v && path && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof seg) return NULL;
        memcpy(seg, path, n);
        seg[n] = 0;
        if (v->type == JV_ARR) {
            int idx = atoi(seg);
            jv * c = v->child;
            while (c && idx-- > 0) c = c->next;
            v = c;
        } else {
            v = jget(v, seg);
        }
        path += n;
        if (*path == '.') path++;
    }
    return (jv *)v;
}

static jv * jfirst(const jv * arr) { return arr && (arr->type == JV_ARR || arr->type == JV_OBJ) ? arr->child : NULL; }

static int jcount(const jv * arr) { int n = 0; for (jv * c = jfirst(arr); c; c = c->next) n++; return n; }

/* String at path (def if missing/not a string). */
static const char * jstr(const jv * v, const char * path, const char * def) {
    jv * x = jpath(v, path);
    return x && x->type == JV_STR ? x->s : def;
}

/* Number at path; numeric strings are accepted too. */
static double jnum(const jv * v, const char * path, double def) {
    jv * x = jpath(v, path);
    if (!x) return def;
    if (x->type == JV_NUM || x->type == JV_BOOL) return x->n;
    if (x->type == JV_STR && x->s[0]) { char * e; double d = strtod(x->s, &e); if (e != x->s) return d; }
    return def;
}

static int jbool(const jv * v, const char * path, int def) {
    jv * x = jpath(v, path);
    if (!x || x->type == JV_NULL) return def;
    if (x->type == JV_BOOL || x->type == JV_NUM) return x->n != 0;
    if (x->type == JV_STR) return !strcmp(x->s, "true") || !strcmp(x->s, "1");
    return def;
}

/* Append s to out as a JSON string literal (with quotes). Returns the new
 * length or -1 when it does not fit. */
static int jesc(char * out, size_t cap, size_t len, const char * s) {
    if (len + 2 >= cap) return -1;
    out[len++] = '"';
    for (const unsigned char * p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char esc[8];
        const char * add;
        if (*p == '"') add = "\\\"";
        else if (*p == '\\') add = "\\\\";
        else if (*p == '\n') add = "\\n";
        else if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); add = esc; }
        else { esc[0] = (char)*p; esc[1] = 0; add = esc; }
        size_t an = strlen(add);
        if (len + an + 2 >= cap) return -1;
        memcpy(out + len, add, an);
        len += an;
    }
    out[len++] = '"';
    out[len] = 0;
    return (int)len;
}

#endif /* FTJSON_H */
