/*
 * screen_teletekst.c — Full-Screen Interactive NOS Teletekst Viewer for FreeToon
 *
 * Provides authentic Dutch Teletext browsing (pages 100-899, subpages,
 * FastText 4-color shortcuts, quick category presets, and on-screen numeric keypad).
 * Features:
 *   - Genuine Bedstead SAA5050 monospace font with full mosaic block graphics (0xEE20-0xEE7F).
 *   - Centered 40-column CRT page layout with no line-wrapping.
 *   - FastText 4-color buttons aligned to exact page width directly below the screen.
 *   - Permanent side panel with 3x4 numeric keypad and 4 glued navigation buttons.
 *   - Clickable page numbers at the end of lines with subtle yellow underline and 1-tap jump.
 */

#define _GNU_SOURCE
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>

#ifdef TOON1
extern const lv_font_t font_teletekst_15;
#define FONT_TELETEKST (&font_teletekst_15)
#define CHAR_W 9
#define LINE_H 16
#define PAD_X  6
#define PAD_Y  6
#else
extern const lv_font_t font_teletekst_20;
#define FONT_TELETEKST (&font_teletekst_20)
#define CHAR_W 12
#define LINE_H 20
#define PAD_X  8
#define PAD_Y  8
#endif

#define MAX_CONTENT_RAW  32768
#define MAX_CONTENT_OUT  65536
#define MAX_LINKS        48
#define MAX_BG_BOXES     24

typedef struct {
    char title[32];
    char page[16];
} fasttext_link_t;

typedef struct {
    char page[8];
    int row;
    int col;
    int len;
} teletekst_link_t;

typedef struct {
    uint32_t bg_color;
    int row;
    int col;
    int len;
} teletekst_bg_t;

typedef struct {
    char page[16];
    char prev_page[16];
    char next_page[16];
    char prev_sub[16];
    char next_sub[16];
    fasttext_link_t fasttext[4];
    int fasttext_count;
    char text[MAX_CONTENT_OUT];
    int is_loading;
    int has_error;
} teletekst_page_t;

static teletekst_page_t s_state;
static pthread_t s_fetch_thread = 0;
static volatile int s_fetch_active = 0;
static char s_req_page[16] = "101";

static teletekst_link_t s_links[MAX_LINKS];
static int s_link_count = 0;
static lv_obj_t * s_link_btns[MAX_LINKS];

static teletekst_bg_t s_bgs[MAX_BG_BOXES];
static int s_bg_count = 0;
static lv_obj_t * s_bg_objs[MAX_BG_BOXES];

static lv_obj_t * s_modal = NULL;
static lv_obj_t * s_content_box = NULL;
static lv_obj_t * s_lbl_content = NULL;

static lv_obj_t * s_btn_prev_page = NULL;
static lv_obj_t * s_lbl_prev_page = NULL;
static lv_obj_t * s_btn_next_page = NULL;
static lv_obj_t * s_lbl_next_page = NULL;
static lv_obj_t * s_btn_prev_sub = NULL;
static lv_obj_t * s_btn_next_sub = NULL;

static lv_obj_t * s_btn_ft[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t * s_lbl_ft[4] = {NULL, NULL, NULL, NULL};

/* Permanent Side-Panel Keypad */
static lv_obj_t * s_lbl_numpad_digits = NULL;
static char s_entered_digits[8] = "";

static lv_timer_t * s_ui_timer = NULL;

/* Forward declarations */
static void teletekst_request_page(const char * page);
static void update_ui_from_state(void);

/* HTML entity definition and lookup table */
typedef struct {
    const char * name;
    uint32_t cp;
} html_entity_t;

static const html_entity_t s_html_entities[] = {
    { "&quot;",   0x0022 },
    { "&amp;",    0x0026 },
    { "&apos;",   0x0027 },
    { "&#39;",    0x0027 },
    { "&lt;",     0x003C },
    { "&gt;",     0x003E },
    { "&nbsp;",   0x0020 },
    { "&euml;",   0x00EB },
    { "&eacute;", 0x00E9 },
    { "&egrave;", 0x00E8 },
    { "&iuml;",   0x00EF },
    { "&oacute;", 0x00F3 },
    { "&ouml;",   0x00F6 },
    { "&auml;",   0x00E4 },
    { "&uuml;",   0x00FC },
    { "&ccedil;", 0x00E7 },
    { "&Euml;",   0x00CB },
    { "&Eacute;", 0x00C9 },
    { "&Egrave;", 0x00C8 },
    { "&Iuml;",   0x00CF },
    { "&Oacute;", 0x00D3 },
    { "&Ouml;",   0x00D6 },
    { "&Auml;",   0x00C4 },
    { "&Uuml;",   0x00DC },
    { "&Ccedil;", 0x00C7 },
    { "&aacute;", 0x00E1 },
    { "&iacute;", 0x00ED },
    { "&uacute;", 0x00FA },
    { "&agrave;", 0x00E0 },
    { "&igrave;", 0x00EC },
    { "&ograve;", 0x00F2 },
    { "&ugrave;", 0x00F9 },
    { "&acirc;",  0x00E2 },
    { "&ecirc;",  0x00EA },
    { "&icirc;",  0x00EE },
    { "&ocirc;",  0x00F4 },
    { "&ucirc;",  0x00FB },
    { "&atilde;", 0x00E3 },
    { "&otilde;", 0x00F5 },
    { "&ntilde;", 0x00F1 },
    { "&Ntilde;", 0x00D1 },
    { "&szlig;",  0x00DF },
    { "&euro;",   0x20AC },
    { "&copy;",   0x00A9 },
    { "&reg;",    0x00AE },
    { "&deg;",    0x00B0 },
    { "&plusmn;", 0x00B1 },
    { "&sup2;",   0x00B2 },
    { "&sup3;",   0x00B3 },
    { "&micro;",  0x00B5 },
    { "&middot;", 0x00B7 },
    { "&frac12;", 0x00BD },
    { "&frac14;", 0x00BC },
    { "&frac34;", 0x00BE },
    { "&iquest;", 0x00BF },
    { "&iexcl;",  0x00A1 },
    { NULL, 0 }
};

static inline size_t encode_utf8(uint32_t cp, char * out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static int decode_html_entity(const char * p, uint32_t * out_cp, int * entity_len) {
    if (*p != '&') return 0;

    /* Hex numeric entity: &#xHH; or &#XHH; */
    if (strncmp(p, "&#x", 3) == 0 || strncmp(p, "&#X", 3) == 0) {
        const char * semi = strchr(p + 3, ';');
        if (semi && semi <= p + 10) {
            char hex_buf[16];
            size_t len = (size_t)(semi - (p + 3));
            memcpy(hex_buf, p + 3, len);
            hex_buf[len] = 0;
            char * end = NULL;
            unsigned long val = strtoul(hex_buf, &end, 16);
            if (end == hex_buf + len) {
                *out_cp = (uint32_t)val;
                *entity_len = (int)(semi - p + 1);
                return 1;
            }
        }
    }

    /* Decimal numeric entity: &#DDD; */
    if (strncmp(p, "&#", 2) == 0 && isdigit((unsigned char)p[2])) {
        const char * semi = strchr(p + 2, ';');
        if (semi && semi <= p + 10) {
            char dec_buf[16];
            size_t len = (size_t)(semi - (p + 2));
            memcpy(dec_buf, p + 2, len);
            dec_buf[len] = 0;
            char * end = NULL;
            unsigned long val = strtoul(dec_buf, &end, 10);
            if (end == dec_buf + len) {
                *out_cp = (uint32_t)val;
                *entity_len = (int)(semi - p + 1);
                return 1;
            }
        }
    }

    /* Named HTML entities */
    for (int i = 0; s_html_entities[i].name != NULL; i++) {
        size_t nlen = strlen(s_html_entities[i].name);
        if (strncmp(p, s_html_entities[i].name, nlen) == 0) {
            *out_cp = s_html_entities[i].cp;
            *entity_len = (int)nlen;
            return 1;
        }
    }

    return 0;
}

static void add_bg_segment(uint32_t bg_col, int row, int col_start, int col_end) {
    if (!bg_col || col_end <= col_start) return;
    int len = col_end - col_start;
    if (s_bg_count > 0 &&
        s_bgs[s_bg_count - 1].row == row &&
        s_bgs[s_bg_count - 1].bg_color == bg_col &&
        s_bgs[s_bg_count - 1].col + s_bgs[s_bg_count - 1].len == col_start) {
        s_bgs[s_bg_count - 1].len += len;
        return;
    }
    if (s_bg_count < MAX_BG_BOXES) {
        s_bgs[s_bg_count].bg_color = bg_col;
        s_bgs[s_bg_count].row = row;
        s_bgs[s_bg_count].col = col_start;
        s_bgs[s_bg_count].len = len;
        s_bg_count++;
    }
}

/* Convert HTML entities and NOS Teletext spans into LVGL recolor tags and Bedstead mosaic UTF-8 */
static void parse_teletekst_content(const char * raw_html, char * out, size_t out_max) {
    size_t oi = 0;
    const char * p = raw_html;
    int in_color = 0;
    int cur_row = 0;
    int cur_col = 0;
    uint32_t cur_bg_col = 0;
    int cur_bg_start_col = 0;
    s_link_count = 0;
    s_bg_count = 0;

    while (*p && oi + 16 < out_max) {
        /* Standard Teletext page is strictly 24 rows (0-23) */
        if (cur_row >= 24) {
            break;
        }

        /* If at start of a line, check if this line is the redundant fastText row */
        if (cur_col == 0) {
            const char * eol = strchr(p, '\n');
            const char * eol2 = strstr(p, "\\n");
            if (eol2 && (!eol || eol2 < eol)) eol = eol2;

            size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
            if (line_len < 1024) {
                char line_peek[1024];
                memcpy(line_peek, p, line_len);
                line_peek[line_len] = 0;
                if (strstr(line_peek, "fastText") != NULL) {
                    /* Omit redundant fastText row from page content */
                    if (eol) {
                        p = (eol == eol2) ? eol + 2 : eol + 1;
                    } else {
                        p += line_len;
                    }
                    continue;
                }
            }
        }

        /* Check span tags with color and background classes */
        if (strncmp(p, "<span class=", 12) == 0 || strncmp(p, "<span class=\\\"", 14) == 0) {
            const char * end_tag = strchr(p, '>');
            if (!end_tag) break;

            char tag_buf[128];
            size_t tlen = (size_t)(end_tag - p + 1);
            if (tlen >= sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
            memcpy(tag_buf, p, tlen);
            tag_buf[tlen] = 0;

            /* Background color parsing (e.g. bg-red, bg-blue) */
            uint32_t new_bg = 0;
            if (strstr(tag_buf, "bg-red"))        new_bg = 0xcc0000;
            else if (strstr(tag_buf, "bg-blue"))  new_bg = 0x0044aa;
            else if (strstr(tag_buf, "bg-green")) new_bg = 0x00aa00;
            else if (strstr(tag_buf, "bg-yellow"))new_bg = 0xccaa00;
            else if (strstr(tag_buf, "bg-cyan"))  new_bg = 0x00aaaa;
            else if (strstr(tag_buf, "bg-white")) new_bg = 0xffffff;

            /* Strip out all bg- color class names so they cannot match foreground checks */
            char fg_buf[128];
            strncpy(fg_buf, tag_buf, sizeof(fg_buf) - 1);
            fg_buf[sizeof(fg_buf) - 1] = 0;
            const char * bg_names[] = { "bg-red", "bg-blue", "bg-green", "bg-yellow", "bg-cyan", "bg-white", NULL };
            for (int bgi = 0; bg_names[bgi]; bgi++) {
                char * p_bg;
                while ((p_bg = strstr(fg_buf, bg_names[bgi])) != NULL) {
                    memset(p_bg, ' ', strlen(bg_names[bgi]));
                }
            }

            const char * hex_col = NULL;
            if (strstr(fg_buf, "yellow"))      hex_col = "#ffff00 ";
            else if (strstr(fg_buf, "cyan"))   hex_col = "#00ffff ";
            else if (strstr(fg_buf, "green"))  hex_col = "#00ff00 ";
            else if (strstr(fg_buf, "red"))    hex_col = "#ff3333 ";
            else if (strstr(fg_buf, "blue"))   hex_col = "#3388ff ";
            else if (strstr(fg_buf, "white"))  hex_col = "#ffffff ";
            else if (new_bg)                   hex_col = "#ffffff "; /* default white text on colored bg */

            if (hex_col) {
                if (in_color) {
                    out[oi++] = '#'; /* close previous color */
                }
                size_t clen = strlen(hex_col);
                memcpy(out + oi, hex_col, clen);
                oi += clen;
                in_color = 1;
            }

            if (new_bg != cur_bg_col) {
                if (cur_bg_col) {
                    add_bg_segment(cur_bg_col, cur_row, cur_bg_start_col, cur_col);
                }
                cur_bg_col = new_bg;
                cur_bg_start_col = cur_col;
            }

            p = end_tag + 1;
            continue;
        }

        if (strncmp(p, "</span>", 7) == 0) {
            if (in_color) {
                out[oi++] = '#';
                in_color = 0;
            }
            if (cur_bg_col) {
                add_bg_segment(cur_bg_col, cur_row, cur_bg_start_col, cur_col);
                cur_bg_col = 0;
            }
            p += 7;
            continue;
        }

        /* Check anchor tag for page links: <a ... href="#(\d{3})"> */
        if (strncmp(p, "<a ", 3) == 0 || strncmp(p, "<a\\", 3) == 0) {
            const char * href = strstr(p, "href=\"#");
            if (!href) href = strstr(p, "href=\\\"#");
            if (href && href < p + 60) {
                const char * num = strchr(href, '#');
                if (num && isdigit((unsigned char)num[1]) && isdigit((unsigned char)num[2]) && isdigit((unsigned char)num[3])) {
                    if (s_link_count < MAX_LINKS) {
                        snprintf(s_links[s_link_count].page, sizeof(s_links[0].page), "%.3s", num + 1);
                        s_links[s_link_count].row = cur_row;
                        s_links[s_link_count].col = cur_col;
                        s_links[s_link_count].len = 3;
                        s_link_count++;
                    }
                }
            }
            const char * end_tag = strchr(p, '>');
            if (end_tag) {
                p = end_tag + 1;
                continue;
            }
        }

        /* Skip closing anchor tags */
        if (strncmp(p, "</a>", 4) == 0) {
            p += 4;
            continue;
        }

        /* Skip other HTML tags */
        if (*p == '<') {
            const char * end_tag = strchr(p, '>');
            if (end_tag) {
                p = end_tag + 1;
                continue;
            }
        }

        /*
         * HTML entities (named like &euml;, numeric &#DDD;, hex &#xHH;,
         * and NOS Teletext mosaic characters &#xF020; to &#xF07F;).
         * In Bedstead, the 64 mosaic block graphics are mapped to 0xEE20 to 0xEE7F.
         */
        uint32_t ent_cp = 0;
        int ent_len = 0;
        if (decode_html_entity(p, &ent_cp, &ent_len)) {
            if (ent_cp >= 0xF020 && ent_cp <= 0xF07F) {
                ent_cp = 0xEE00 + (ent_cp & 0xFF);
            }
            if (ent_cp == '#') {
                out[oi++] = '#';
                out[oi++] = '#';
            } else {
                oi += encode_utf8(ent_cp, out + oi);
            }
            p += ent_len;
            cur_col++;
            continue;
        }

        /* Unescape JSON newlines */
        if (*p == '\\' && *(p + 1) == 'n') {
            if (cur_bg_col) {
                add_bg_segment(cur_bg_col, cur_row, cur_bg_start_col, cur_col);
                cur_bg_start_col = 0;
            }
            out[oi++] = '\n';
            p += 2;
            cur_row++;
            cur_col = 0;
            continue;
        }
        if (*p == '\\' && *(p + 1) == 'r') {
            p += 2;
            continue;
        }
        if (*p == '\\' && *(p + 1) == '"') {
            out[oi++] = '"';
            p += 2;
            cur_col++;
            continue;
        }
        if (*p == '\n') {
            if (cur_bg_col) {
                add_bg_segment(cur_bg_col, cur_row, cur_bg_start_col, cur_col);
                cur_bg_start_col = 0;
            }
            out[oi++] = '\n';
            p++;
            cur_row++;
            cur_col = 0;
            continue;
        }

        /* Escape literal '#' so LVGL recolor doesn't misinterpret */
        if (*p == '#') {
            out[oi++] = '#';
            out[oi++] = '#';
            p++;
            cur_col++;
            continue;
        }

        /* UTF-8 continuation byte: copy byte but do not increment cur_col */
        if (((unsigned char)*p & 0xC0) == 0x80) {
            out[oi++] = *p++;
            continue;
        }

        out[oi++] = *p++;
        cur_col++;
    }

    if (cur_bg_col) {
        add_bg_segment(cur_bg_col, cur_row, cur_bg_start_col, cur_col);
        cur_bg_col = 0;
    }

    if (in_color) {
        out[oi++] = '#';
    }
    out[oi] = 0;
}

static void extract_json_val(const char * json, const char * key, char * out, size_t out_sz) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char * start = strstr(json, pat);
    if (!start) return;
    start += strlen(pat);
    const char * end = strchr(start, '"');
    if (!end) return;
    size_t len = (size_t)(end - start);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = 0;
}

static void * fetch_thread_func(void * arg) {
    (void)arg;
    char page[16];
    snprintf(page, sizeof page, "%s", s_req_page);

    char url[128];
    snprintf(url, sizeof url, "https://teletekst-data.nos.nl/json/%s", page);

    char cmd[256];
    snprintf(cmd, sizeof cmd, "/usr/bin/curl -s -4 -k --max-time 6 --connect-timeout 4 '%s'", url);

    FILE * fp = popen(cmd, "r");
    if (!fp) {
        s_state.is_loading = 0;
        s_state.has_error = 1;
        s_fetch_active = 0;
        return NULL;
    }

    char * buf = malloc(MAX_CONTENT_RAW);
    if (!buf) {
        pclose(fp);
        s_state.is_loading = 0;
        s_state.has_error = 1;
        s_fetch_active = 0;
        return NULL;
    }

    size_t n = fread(buf, 1, MAX_CONTENT_RAW - 1, fp);
    pclose(fp);
    buf[n] = 0;

    if (n < 50) {
        free(buf);
        s_state.is_loading = 0;
        s_state.has_error = 1;
        s_fetch_active = 0;
        return NULL;
    }

    snprintf(s_state.page, sizeof(s_state.page), "%s", page);
    extract_json_val(buf, "prevPage", s_state.prev_page, sizeof(s_state.prev_page));
    extract_json_val(buf, "nextPage", s_state.next_page, sizeof(s_state.next_page));
    extract_json_val(buf, "prevSubPage", s_state.prev_sub, sizeof(s_state.prev_sub));
    extract_json_val(buf, "nextSubPage", s_state.next_sub, sizeof(s_state.next_sub));

    /* Parse FastText links */
    s_state.fasttext_count = 0;
    const char * ft_start = strstr(buf, "\"fastTextLinks\":[");
    if (ft_start) {
        const char * p = ft_start + 17;
        while (s_state.fasttext_count < 4) {
            const char * o_start = strchr(p, '{');
            if (!o_start) break;
            const char * o_end = strchr(o_start, '}');
            if (!o_end) break;

            char item_buf[256];
            size_t ilen = (size_t)(o_end - o_start + 1);
            if (ilen >= sizeof(item_buf)) ilen = sizeof(item_buf) - 1;
            memcpy(item_buf, o_start, ilen);
            item_buf[ilen] = 0;

            extract_json_val(item_buf, "title", s_state.fasttext[s_state.fasttext_count].title, sizeof(s_state.fasttext[0].title));
            extract_json_val(item_buf, "page", s_state.fasttext[s_state.fasttext_count].page, sizeof(s_state.fasttext[0].page));
            s_state.fasttext_count++;

            p = o_end + 1;
            const char * next_arr = strchr(p, ']');
            const char * next_obj = strchr(p, '{');
            if (next_arr && (!next_obj || next_arr < next_obj)) break;
        }
    }

    /* Parse content */
    const char * c_start = strstr(buf, "\"content\":\"");
    if (c_start) {
        c_start += 11;
        const char * c_end = c_start;
        while (*c_end) {
            if (*c_end == '"' && *(c_end - 1) != '\\') break;
            c_end++;
        }
        size_t clen = (size_t)(c_end - c_start);
        char * raw_content = malloc(clen + 1);
        if (raw_content) {
            memcpy(raw_content, c_start, clen);
            raw_content[clen] = 0;
            parse_teletekst_content(raw_content, s_state.text, sizeof(s_state.text));
            free(raw_content);
        }
    } else {
        snprintf(s_state.text, sizeof(s_state.text), "#ff3333 Geen teletekstinhoud gevonden.#");
    }

    free(buf);
    s_state.has_error = 0;
    s_state.is_loading = 0;
    s_fetch_active = 0;
    return NULL;
}

static void on_page_link_click(lv_event_t * e) {
    const char * p = (const char *)lv_event_get_user_data(e);
    if (p && p[0]) teletekst_request_page(p);
}

static void teletekst_request_page(const char * page) {
    if (!page || !page[0]) return;
    snprintf(s_req_page, sizeof s_req_page, "%s", page);
    s_state.is_loading = 1;
    s_state.has_error = 0;

    /* Reset keypad digits display to reflect requested page */
    s_entered_digits[0] = 0;
    if (s_lbl_numpad_digits) {
        char disp[32];
        snprintf(disp, sizeof disp, "Pagina:  #ffd60a %s#", page);
        lv_label_set_text(s_lbl_numpad_digits, disp);
    }

    if (s_lbl_content) {
        char load_msg[128];
        snprintf(load_msg, sizeof load_msg, "#ffff00 Pagina %s laden...#", page);
        lv_label_set_text(s_lbl_content, load_msg);
    }

    /* Clear old line-link buttons and background banners while loading */
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_link_btns[i]) {
            lv_obj_del(s_link_btns[i]);
            s_link_btns[i] = NULL;
        }
    }
    for (int i = 0; i < MAX_BG_BOXES; i++) {
        if (s_bg_objs[i]) {
            lv_obj_del(s_bg_objs[i]);
            s_bg_objs[i] = NULL;
        }
    }

    if (!s_fetch_active) {
        s_fetch_active = 1;
        if (s_ui_timer) {
            lv_timer_resume(s_ui_timer);
        }
        pthread_create(&s_fetch_thread, NULL, fetch_thread_func, NULL);
        pthread_detach(s_fetch_thread);
    }
}

static void update_ui_from_state(void) {
    if (!s_modal) return;
    if (s_state.is_loading) return;

    if (s_state.has_error) {
        if (s_lbl_content) {
            char err_msg[128];
            snprintf(err_msg, sizeof err_msg, "#ff3333 Pagina %s niet beschikbaar of time-out.#", s_req_page);
            lv_label_set_text(s_lbl_content, err_msg);
        }
        return;
    }

    /* Update text body */
    if (s_lbl_content) {
        lv_label_set_text(s_lbl_content, s_state.text);
    }

    /* Clear and recreate colored background boxes (drawn behind text) */
    for (int i = 0; i < MAX_BG_BOXES; i++) {
        if (s_bg_objs[i]) {
            lv_obj_del(s_bg_objs[i]);
            s_bg_objs[i] = NULL;
        }
    }

    if (s_content_box) {
        for (int i = 0; i < s_bg_count; i++) {
            int bx = s_bgs[i].col * CHAR_W;
            int by = s_bgs[i].row * LINE_H;
            int bw = s_bgs[i].len * CHAR_W;
            int bh = LINE_H;

            lv_obj_t * bg = lv_obj_create(s_content_box);
            lv_obj_remove_style_all(bg);
            lv_obj_set_size(bg, bw, bh);
            lv_obj_set_pos(bg, bx, by);
            lv_obj_set_style_bg_color(bg, lv_color_hex(s_bgs[i].bg_color), 0);
            lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bg, 0, 0);
            lv_obj_set_style_radius(bg, 0, 0);
            lv_obj_set_style_pad_all(bg, 0, 0);
            lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_background(bg);
            s_bg_objs[i] = bg;
        }
    }

    /* Create clickable link overlays for page numbers detected in text (transparent hitboxes) */
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_link_btns[i]) {
            lv_obj_del(s_link_btns[i]);
            s_link_btns[i] = NULL;
        }
    }

    if (s_content_box) {
        for (int i = 0; i < s_link_count; i++) {
            int lx = s_links[i].col * CHAR_W;
            int ly = s_links[i].row * LINE_H;
            int lw = s_links[i].len * CHAR_W;
            int lh = LINE_H;

            lv_obj_t * b = lv_btn_create(s_content_box);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, lw, lh);
            lv_obj_set_pos(b, lx, ly);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 2, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_ext_click_area(b, 4);

            /* Highlight feedback on press */
            lv_obj_set_style_bg_color(b, lv_color_hex(0xffd60a), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);

            lv_obj_add_event_cb(b, on_page_link_click, LV_EVENT_CLICKED, (void*)s_links[i].page);
            s_link_btns[i] = b;
        }
    }

    /* Update page navigation buttons */
    if (s_btn_prev_page && s_lbl_prev_page) {
        if (s_state.prev_page[0]) {
            char b[32];
            snprintf(b, sizeof b, "<< %s", s_state.prev_page);
            lv_label_set_text(s_lbl_prev_page, b);
            lv_obj_clear_state(s_btn_prev_page, LV_STATE_DISABLED);
        } else {
            lv_label_set_text(s_lbl_prev_page, "<< Vorige");
            lv_obj_add_state(s_btn_prev_page, LV_STATE_DISABLED);
        }
    }
    if (s_btn_next_page && s_lbl_next_page) {
        if (s_state.next_page[0]) {
            char b[32];
            snprintf(b, sizeof b, "%s >>", s_state.next_page);
            lv_label_set_text(s_lbl_next_page, b);
            lv_obj_clear_state(s_btn_next_page, LV_STATE_DISABLED);
        } else {
            lv_label_set_text(s_lbl_next_page, "Volgende >>");
            lv_obj_add_state(s_btn_next_page, LV_STATE_DISABLED);
        }
    }

    /* Subpage buttons */
    if (s_btn_prev_sub) {
        if (s_state.prev_sub[0]) lv_obj_clear_state(s_btn_prev_sub, LV_STATE_DISABLED);
        else lv_obj_add_state(s_btn_prev_sub, LV_STATE_DISABLED);
    }
    if (s_btn_next_sub) {
        if (s_state.next_sub[0]) lv_obj_clear_state(s_btn_next_sub, LV_STATE_DISABLED);
        else lv_obj_add_state(s_btn_next_sub, LV_STATE_DISABLED);
    }

    /* FastText 4 Color buttons */
    for (int i = 0; i < 4; i++) {
        if (s_btn_ft[i] && s_lbl_ft[i]) {
            if (i < s_state.fasttext_count && s_state.fasttext[i].page[0]) {
                char ft_title[48];
                snprintf(ft_title, sizeof ft_title, "%s (%s)",
                         s_state.fasttext[i].title, s_state.fasttext[i].page);
                lv_label_set_text(s_lbl_ft[i], ft_title);
                lv_obj_clear_flag(s_btn_ft[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_btn_ft[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void on_ui_poll(lv_timer_t * timer) {
    if (!s_state.is_loading) {
        update_ui_from_state();
        lv_timer_pause(timer); /* Stop timer once loaded — 0% idle CPU */
    }
}

/* Event Handlers */
static void on_close_click(lv_event_t * e) {
    (void)e;
    ui_pop();
}

static void on_refresh_click(lv_event_t * e) {
    (void)e;
    teletekst_request_page(s_state.page[0] ? s_state.page : "101");
}

static void on_prev_page_click(lv_event_t * e) {
    (void)e;
    if (s_state.prev_page[0]) teletekst_request_page(s_state.prev_page);
}

static void on_next_page_click(lv_event_t * e) {
    (void)e;
    if (s_state.next_page[0]) teletekst_request_page(s_state.next_page);
}

static void on_prev_sub_click(lv_event_t * e) {
    (void)e;
    if (s_state.prev_sub[0]) teletekst_request_page(s_state.prev_sub);
}

static void on_next_sub_click(lv_event_t * e) {
    (void)e;
    if (s_state.next_sub[0]) teletekst_request_page(s_state.next_sub);
}

static void on_fasttext_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_state.fasttext_count) {
        teletekst_request_page(s_state.fasttext[idx].page);
    }
}

/* Permanent Side-Panel Keypad Event Handlers */
static void on_panel_numpad_btn(lv_event_t * e) {
    const char * txt = (const char *)lv_event_get_user_data(e);
    if (!txt) return;

    if (strcmp(txt, "DEL") == 0) {
        size_t len = strlen(s_entered_digits);
        if (len > 0) s_entered_digits[len - 1] = 0;
    } else if (strcmp(txt, "GO") == 0) {
        if (s_entered_digits[0]) {
            teletekst_request_page(s_entered_digits);
            s_entered_digits[0] = 0;
            return;
        }
    } else {
        size_t len = strlen(s_entered_digits);
        if (len < 3) {
            s_entered_digits[len] = txt[0];
            s_entered_digits[len + 1] = 0;
            if (len + 1 == 3) {
                teletekst_request_page(s_entered_digits);
                s_entered_digits[0] = 0;
                return;
            }
        }
    }

    if (s_lbl_numpad_digits) {
        char disp[48];
        if (s_entered_digits[0]) {
            snprintf(disp, sizeof disp, "Pagina:  #ffd60a [%-3s]#", s_entered_digits);
        } else {
            snprintf(disp, sizeof disp, "Pagina:  #ffd60a %s#", s_state.page[0] ? s_state.page : "101");
        }
        lv_label_set_text(s_lbl_numpad_digits, disp);
    }
}

static void on_modal_deleted(lv_event_t * e) {
    (void)e;
    if (s_ui_timer) {
        lv_timer_del(s_ui_timer);
        s_ui_timer = NULL;
    }
    s_modal = NULL;
    s_content_box = NULL;
    s_lbl_content = NULL;
    s_btn_prev_page = NULL;
    s_btn_next_page = NULL;
    s_btn_prev_sub = NULL;
    s_btn_next_sub = NULL;
    for (int i = 0; i < 4; i++) {
        s_btn_ft[i] = NULL;
        s_lbl_ft[i] = NULL;
    }
    for (int i = 0; i < MAX_LINKS; i++) {
        s_link_btns[i] = NULL;
    }
    s_link_count = 0;
    for (int i = 0; i < MAX_BG_BOXES; i++) {
        s_bg_objs[i] = NULL;
    }
    s_bg_count = 0;
    s_lbl_numpad_digits = NULL;
}

/* Entry point exported to FreeToon core */
lv_obj_t * screen_create(void) {
    if (s_modal) lv_obj_del(s_modal);

    /* Compute exact 40-column page dimensions */
    int content_w = 40 * CHAR_W + PAD_X * 2;
    int content_h = 24 * LINE_H + PAD_Y * 2;

    s_modal = lv_obj_create(NULL);
    lv_obj_add_event_cb(s_modal, on_modal_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(s_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Close & Refresh buttons in top-right corner (large touch targets matching FreeToon modal style) */
    int x_w = (LV_VER_RES < 600) ? 60 : 70;
    int x_h = (LV_VER_RES < 600) ? 44 : 50;

    lv_obj_t * b_close = lv_btn_create(s_modal);
    lv_obj_set_size(b_close, x_w, x_h);
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(b_close, lv_color_hex(0x28282b), 0);
    lv_obj_set_style_radius(b_close, 10, 0);
    lv_obj_set_ext_click_area(b_close, 20);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_close = lv_label_create(b_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_close, SF(22), 0);
    lv_obj_center(lbl_close);

    lv_obj_t * b_ref = lv_btn_create(s_modal);
    lv_obj_set_size(b_ref, x_w - SX(6), x_h);
    lv_obj_align(b_ref, LV_ALIGN_TOP_RIGHT, SX(-12) - x_w - SX(8), SY(8));
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(0x28282b), 0);
    lv_obj_set_style_radius(b_ref, 10, 0);
    lv_obj_set_ext_click_area(b_ref, 20);
    lv_obj_add_event_cb(b_ref, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ref = lv_label_create(b_ref);
    lv_label_set_text(lbl_ref, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_ref, SF(20), 0);
    lv_obj_center(lbl_ref);

    /* --- Left Panel: CRT Teletext Content Box --- */
    int ft_h = (LV_VER_RES < 600) ? 26 : 32;
    int ft_gap_y = SY(4);
    int total_screen_h = content_h + ft_gap_y + ft_h;
    int top_y = LV_VER_RES - total_screen_h - SY(10);
    if (top_y < SY(56)) top_y = SY(56);
    int left_x = SX(12);

    s_content_box = lv_obj_create(s_modal);
    lv_obj_set_size(s_content_box, content_w, content_h);
    lv_obj_set_pos(s_content_box, left_x, top_y);
    lv_obj_set_style_bg_color(s_content_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(s_content_box, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_border_width(s_content_box, 1, 0);
    lv_obj_set_style_radius(s_content_box, 4, 0);
    lv_obj_set_style_pad_left(s_content_box, PAD_X, 0);
    lv_obj_set_style_pad_right(s_content_box, PAD_X, 0);
    lv_obj_set_style_pad_top(s_content_box, PAD_Y, 0);
    lv_obj_set_style_pad_bottom(s_content_box, PAD_Y, 0);
    lv_obj_clear_flag(s_content_box, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_content = lv_label_create(s_content_box);
    lv_label_set_recolor(s_lbl_content, true);
    lv_label_set_long_mode(s_lbl_content, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_lbl_content, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_content, FONT_TELETEKST, 0);
    lv_obj_set_style_pad_all(s_lbl_content, 0, 0);
    lv_obj_set_style_border_width(s_lbl_content, 0, 0);
    lv_obj_set_width(s_lbl_content, 40 * CHAR_W + 4);
    lv_obj_align(s_lbl_content, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_content, "#ffd60a Teletekst laden...#");

    /* --- Left Panel: FastText 4 Color Buttons Aligned to Exact Page Width Below --- */
    int ft_y = top_y + content_h + ft_gap_y;

    uint32_t ft_colors[4] = { 0xcc0000, 0x008800, 0xccaa00, 0x0066cc };
    uint32_t ft_text_colors[4] = { 0xffffff, 0xffffff, 0x000000, 0xffffff };
    int ft_gap = SX(4);
    int ft_w = (content_w - (3 * ft_gap)) / 4;

    for (int i = 0; i < 4; i++) {
        s_btn_ft[i] = lv_btn_create(s_modal);
        lv_obj_set_size(s_btn_ft[i], ft_w, ft_h);
        lv_obj_set_pos(s_btn_ft[i], left_x + i * (ft_w + ft_gap), ft_y);
        lv_obj_set_style_bg_color(s_btn_ft[i], lv_color_hex(ft_colors[i]), 0);
        lv_obj_set_style_radius(s_btn_ft[i], 4, 0);
        lv_obj_set_style_pad_all(s_btn_ft[i], 0, 0);
        lv_obj_add_event_cb(s_btn_ft[i], on_fasttext_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_flag(s_btn_ft[i], LV_OBJ_FLAG_HIDDEN);

        s_lbl_ft[i] = lv_label_create(s_btn_ft[i]);
        lv_obj_set_style_text_color(s_lbl_ft[i], lv_color_hex(ft_text_colors[i]), 0);
        lv_obj_set_style_text_font(s_lbl_ft[i], SF(11), 0);
        lv_label_set_text(s_lbl_ft[i], "");
        lv_obj_center(s_lbl_ft[i]);
    }

    /* --- Right Panel: Data Entry Panel (Keypad + Glued Scroll Navigation) --- */
    int panel_x = left_x + content_w + SX(12);
    int panel_w = LV_HOR_RES - panel_x - SX(12);
    int panel_h = total_screen_h;

    lv_obj_t * side_panel = lv_obj_create(s_modal);
    lv_obj_set_size(side_panel, panel_w, panel_h);
    lv_obj_set_pos(side_panel, panel_x, top_y);
    lv_obj_set_style_bg_color(side_panel, lv_color_hex(0x141416), 0);
    lv_obj_set_style_border_color(side_panel, lv_color_hex(0x28282a), 0);
    lv_obj_set_style_border_width(side_panel, 1, 0);
    lv_obj_set_style_radius(side_panel, 8, 0);
    lv_obj_set_style_pad_all(side_panel, 0, 0);
    lv_obj_clear_flag(side_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Dimensions for Enlarged Numpad (about 50% wider and taller) and Centered Control Block */
    int key_w = SX(68);          /* 68px on Toon 1 (~50% wider than previous 46px), 87px on Toon 2 */
    int key_h = SY(60);          /* 60px on Toon 1 (~35% taller), 75px on Toon 2 */
    int key_gap_x = SX(8);
    int key_gap_y = SY(5);
    int block_w = 3 * key_w + 2 * key_gap_x;

    int disp_h = SY(36);
    int gap_disp = SY(8);
    int keypad_h = 4 * key_h + 3 * key_gap_y;
    int gap_nav = SY(8);
    int nav_w = (block_w - key_gap_x) / 2;
    int nav_h = SY(38);
    int nav_gap_y = SY(6);
    int nav_h_total = 2 * nav_h + nav_gap_y;
    int total_block_h = disp_h + gap_disp + keypad_h + gap_nav + nav_h_total;

    int start_x = (panel_w - block_w) / 2;
    if (start_x < 0) start_x = 0;
    int start_y = (panel_h - total_block_h) / 2;
    if (start_y < SY(4)) start_y = SY(4);

    /* 1. Page Display */
    lv_obj_t * disp_box = lv_obj_create(side_panel);
    lv_obj_set_size(disp_box, block_w, disp_h);
    lv_obj_set_pos(disp_box, start_x, start_y);
    lv_obj_set_style_bg_color(disp_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(disp_box, lv_color_hex(0x3a3a3c), 0);
    lv_obj_set_style_border_width(disp_box, 1, 0);
    lv_obj_set_style_radius(disp_box, 6, 0);
    lv_obj_set_style_pad_all(disp_box, 0, 0);
    lv_obj_clear_flag(disp_box, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_numpad_digits = lv_label_create(disp_box);
    lv_label_set_recolor(s_lbl_numpad_digits, true);
    lv_obj_set_style_text_font(s_lbl_numpad_digits, SF(17), 0);
    lv_label_set_text(s_lbl_numpad_digits, "Pagina:  #ffd60a 101#");
    lv_obj_center(s_lbl_numpad_digits);

    /* 2. Numeric Keypad 3x4 Grid (Enlarged buttons) */
    const char * keys[12] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "DEL", "0", "GO"
    };

    int key_start_y = start_y + disp_h + gap_disp;

    for (int i = 0; i < 12; i++) {
        int r = i / 3;
        int c = i % 3;
        lv_obj_t * b = lv_btn_create(side_panel);
        lv_obj_set_size(b, key_w, key_h);
        lv_obj_set_pos(b, start_x + c * (key_w + key_gap_x), key_start_y + r * (key_h + key_gap_y));

        if (strcmp(keys[i], "GO") == 0) {
            lv_obj_set_style_bg_color(b, lv_color_hex(0x28cd41), 0);
        } else if (strcmp(keys[i], "DEL") == 0) {
            lv_obj_set_style_bg_color(b, lv_color_hex(0xff3b30), 0);
        } else {
            lv_obj_set_style_bg_color(b, lv_color_hex(0x28282b), 0);
        }
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_panel_numpad_btn, LV_EVENT_CLICKED, (void*)keys[i]);

        lv_obj_t * lbl = lv_label_create(b);
        lv_label_set_text(lbl, (strcmp(keys[i], "DEL") == 0) ? "<" : ((strcmp(keys[i], "GO") == 0) ? "OK" : keys[i]));
        lv_obj_set_style_text_font(lbl, SF(20), 0);
        lv_obj_center(lbl);
    }

    /* 3. Four Glued Navigation / Scroll Buttons Below Keypad */
    int nav_start_y = key_start_y + keypad_h + gap_nav;

    /* Row 1: Subpage buttons (< Deel / Deel >) */
    s_btn_prev_sub = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_prev_sub, nav_w, nav_h);
    lv_obj_set_pos(s_btn_prev_sub, start_x, nav_start_y);
    lv_obj_set_style_bg_color(s_btn_prev_sub, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_prev_sub, 6, 0);
    lv_obj_set_style_pad_all(s_btn_prev_sub, 0, 0);
    lv_obj_add_event_cb(s_btn_prev_sub, on_prev_sub_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ps = lv_label_create(s_btn_prev_sub);
    lv_label_set_text(lbl_ps, "< Deel");
    lv_obj_set_style_text_font(lbl_ps, SF(14), 0);
    lv_obj_center(lbl_ps);

    s_btn_next_sub = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_next_sub, nav_w, nav_h);
    lv_obj_set_pos(s_btn_next_sub, start_x + nav_w + key_gap_x, nav_start_y);
    lv_obj_set_style_bg_color(s_btn_next_sub, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_next_sub, 6, 0);
    lv_obj_set_style_pad_all(s_btn_next_sub, 0, 0);
    lv_obj_add_event_cb(s_btn_next_sub, on_next_sub_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ns = lv_label_create(s_btn_next_sub);
    lv_label_set_text(lbl_ns, "Deel >");
    lv_obj_set_style_text_font(lbl_ns, SF(14), 0);
    lv_obj_center(lbl_ns);

    /* Row 2: Page Steppers (<< Vorige / Volgende >>) */
    int page_nav_y = nav_start_y + nav_h + nav_gap_y;

    s_btn_prev_page = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_prev_page, nav_w, nav_h);
    lv_obj_set_pos(s_btn_prev_page, start_x, page_nav_y);
    lv_obj_set_style_bg_color(s_btn_prev_page, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_prev_page, 6, 0);
    lv_obj_set_style_pad_all(s_btn_prev_page, 0, 0);
    lv_obj_add_event_cb(s_btn_prev_page, on_prev_page_click, LV_EVENT_CLICKED, NULL);

    s_lbl_prev_page = lv_label_create(s_btn_prev_page);
    lv_label_set_text(s_lbl_prev_page, "<< Vorige");
    lv_obj_set_style_text_font(s_lbl_prev_page, SF(14), 0);
    lv_obj_center(s_lbl_prev_page);

    s_btn_next_page = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_next_page, nav_w, nav_h);
    lv_obj_set_pos(s_btn_next_page, start_x + nav_w + key_gap_x, page_nav_y);
    lv_obj_set_style_bg_color(s_btn_next_page, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_next_page, 6, 0);
    lv_obj_set_style_pad_all(s_btn_next_page, 0, 0);
    lv_obj_add_event_cb(s_btn_next_page, on_next_page_click, LV_EVENT_CLICKED, NULL);

    s_lbl_next_page = lv_label_create(s_btn_next_page);
    lv_label_set_text(s_lbl_next_page, "Volgende >>");
    lv_obj_set_style_text_font(s_lbl_next_page, SF(14), 0);
    lv_obj_center(s_lbl_next_page);

    /* Timer for async updates (active only during page fetch, paused once loaded) */
    s_ui_timer = lv_timer_create(on_ui_poll, 200, NULL);

    /* Initial fetch: Page 101 */
    teletekst_request_page(s_state.page[0] ? s_state.page : "101");

    return s_modal;
}
