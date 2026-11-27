/*
 * matter_json.c - see matter_json.h. Recursive descent, depth-limited.
 */
#include "matter_json.h"

#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH 32

typedef struct {
    const char * p;
    int          depth;
} mj_parser_t;

static void skip_ws(mj_parser_t * ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static mj_t * node_new(mj_type_t type) {
    mj_t * n = calloc(1, sizeof *n);
    if (n) n->type = type;
    return n;
}

void mj_free(mj_t * node) {
    while (node) {
        mj_t * next = node->next;
        mj_free(node->child);
        free(node->key);
        free(node->str);
        free(node);
        node = next;
    }
}

static int hex4(const char * p, unsigned * out) {
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

static size_t put_utf8(char * o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f)); return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* ps->p is at the opening quote. The result is never longer than the input. */
static char * parse_string(mj_parser_t * ps) {
    const char * start = ++ps->p;
    size_t cap = 0;
    for (const char * q = start; *q && *q != '"'; q++) { if (*q == '\\' && q[1]) q++; cap++; }
    char * out = malloc(cap * 4 + 1);
    if (!out) return NULL;
    size_t n = 0;
    while (*ps->p && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p;
        if (c < 0x20) { free(out); return NULL; }
        if (c != '\\') { out[n++] = (char)c; ps->p++; continue; }
        ps->p++;
        switch (*ps->p) {
        case '"': out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; break;
        case '/': out[n++] = '/'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (hex4(ps->p + 1, &cp) != 0) { free(out); return NULL; }
            ps->p += 4;
            if (cp >= 0xd800 && cp <= 0xdbff && ps->p[1] == '\\' && ps->p[2] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 3, &lo) == 0 && lo >= 0xdc00 && lo <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    ps->p += 6;
                }
            }
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;   /* lone surrogate */
            n += put_utf8(out + n, cp);
            break;
        }
        default: free(out); return NULL;
        }
        ps->p++;
    }
    if (*ps->p != '"') { free(out); return NULL; }
    ps->p++;
    out[n] = 0;
    return out;
}

static mj_t * parse_value(mj_parser_t * ps);

static mj_t * parse_container(mj_parser_t * ps, int is_obj) {
    if (++ps->depth > MJ_MAX_DEPTH) return NULL;
    mj_t * node = node_new(is_obj ? MJ_OBJ : MJ_ARR);
    if (!node) return NULL;
    ps->p++;
    skip_ws(ps);
    char close = is_obj ? '}' : ']';
    if (*ps->p == close) { ps->p++; ps->depth--; return node; }
    mj_t ** tail = &node->child;
    for (;;) {
        char * key = NULL;
        skip_ws(ps);
        if (is_obj) {
            if (*ps->p != '"' || !(key = parse_string(ps))) goto fail;
            skip_ws(ps);
            if (*ps->p != ':') { free(key); goto fail; }
            ps->p++;
        }
        mj_t * v = parse_value(ps);
        if (!v) { free(key); goto fail; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; break; }
        goto fail;
    }
    ps->depth--;
    return node;
fail:
    mj_free(node);
    return NULL;
}

static mj_t * parse_number(mj_parser_t * ps) {
    const char * s = ps->p;
    if (*s == '-') s++;
    if (*s < '0' || *s > '9') return NULL;
    char * end;
    double v = strtod(ps->p, &end);
    if (end == ps->p) return NULL;
    mj_t * n = node_new(MJ_NUM);
    if (!n) return NULL;
    n->num = v;
    ps->p = end;
    return n;
}

static mj_t * parse_value(mj_parser_t * ps) {
    skip_ws(ps);
    switch (*ps->p) {
    case '{': return parse_container(ps, 1);
    case '[': return parse_container(ps, 0);
    case '"': {
        char * s = parse_string(ps);
        if (!s) return NULL;
        mj_t * n = node_new(MJ_STR);
        if (!n) { free(s); return NULL; }
        n->str = s;
        return n;
    }
    case 't':
        if (strncmp(ps->p, "true", 4) == 0) { ps->p += 4; mj_t * n = node_new(MJ_BOOL); if (n) n->boolean = 1; return n; }
        return NULL;
    case 'f':
        if (strncmp(ps->p, "false", 5) == 0) { ps->p += 5; return node_new(MJ_BOOL); }
        return NULL;
    case 'n':
        if (strncmp(ps->p, "null", 4) == 0) { ps->p += 4; return node_new(MJ_NULL); }
        return NULL;
    default:
        return parse_number(ps);
    }
}

mj_t * mj_parse(const char * text) {
    if (!text) return NULL;
    mj_parser_t ps = { text, 0 };
    if ((unsigned char)ps.p[0] == 0xef && (unsigned char)ps.p[1] == 0xbb && (unsigned char)ps.p[2] == 0xbf)
        ps.p += 3;                                   /* UTF-8 BOM */
    mj_t * root = parse_value(&ps);
    if (!root) return NULL;
    skip_ws(&ps);
    if (*ps.p) { mj_free(root); return NULL; }
    return root;
}

const mj_t * mj_get(const mj_t * obj, const char * key) {
    if (!obj || obj->type != MJ_OBJ || !key) return NULL;
    for (const mj_t * c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

const char * mj_str(const mj_t * node, const char * fallback) {
    return node && node->type == MJ_STR ? node->str : fallback;
}

double mj_num(const mj_t * node, double fallback) {
    return node && node->type == MJ_NUM ? node->num : fallback;
}

int mj_is_true(const mj_t * node) {
    return node && node->type == MJ_BOOL && node->boolean;
}

size_t mj_quote(const char * s, char * out, size_t n) {
    size_t need = 0;
#define PUT(ch) do { if (need + 1 < n) out[need] = (ch); need++; } while (0)
    PUT('"');
    for (const unsigned char * p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { PUT('\\'); PUT((char)c); }
        else if (c == '\n') { PUT('\\'); PUT('n'); }
        else if (c == '\r') { PUT('\\'); PUT('r'); }
        else if (c == '\t') { PUT('\\'); PUT('t'); }
        else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hex[c >> 4]); PUT(hex[c & 15]);
        } else PUT((char)c);
    }
    PUT('"');
#undef PUT
    if (n) out[need < n ? need : n - 1] = 0;
    return need;
}
