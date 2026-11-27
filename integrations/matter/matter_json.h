/*
 * matter_json.h - a small JSON reader for the Matter integration.
 *
 * toonui does not export a JSON library to plugins, and the files this
 * integration reads (the bridge's status.json, the device preferences and the
 * ComfortHub state) are a few KB at most, so a tiny tree parser is enough.
 * Shared by the daemon and the UI plugin; unit-tested on the host.
 */
#ifndef MATTER_JSON_H
#define MATTER_JSON_H

#include <stddef.h>

typedef enum { MJ_NULL = 0, MJ_BOOL, MJ_NUM, MJ_STR, MJ_ARR, MJ_OBJ } mj_type_t;

typedef struct mj {
    mj_type_t   type;
    char *      key;          /* member name when the parent is an object */
    char *      str;          /* MJ_STR: UTF-8, unescaped */
    double      num;          /* MJ_NUM */
    int         boolean;      /* MJ_BOOL */
    struct mj * child;        /* MJ_ARR / MJ_OBJ: first element */
    struct mj * next;         /* next sibling */
} mj_t;

/* Parses a complete document; NULL on any syntax error (or trailing junk). */
mj_t *       mj_parse(const char * text);
void         mj_free(mj_t * node);

/* Object member by name (first match), NULL if absent or not an object. */
const mj_t * mj_get(const mj_t * obj, const char * key);

/* Typed accessors with a fallback for a missing or wrongly typed node. */
const char * mj_str(const mj_t * node, const char * fallback);
double       mj_num(const mj_t * node, double fallback);
int          mj_is_true(const mj_t * node);          /* only a JSON `true` */

/* Appends `s` to `out` as a JSON string literal (with quotes). Returns the
 * number of bytes the full literal needs, like snprintf. */
size_t       mj_quote(const char * s, char * out, size_t n);

#endif
