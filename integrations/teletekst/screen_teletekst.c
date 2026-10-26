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
#define LINE_H 15
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

static lv_obj_t * s_modal = NULL;
static lv_obj_t * s_lbl_header_page = NULL;
static lv_obj_t * s_lbl_sub_info = NULL;
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

/* Convert HTML entities and NOS Teletext spans into LVGL recolor tags and Bedstead mosaic UTF-8 */
static void parse_teletekst_content(const char * raw_html, char * out, size_t out_max) {
    size_t oi = 0;
    const char * p = raw_html;
    int in_color = 0;
    int cur_row = 0;
    int cur_col = 0;
    s_link_count = 0;

    while (*p && oi + 16 < out_max) {
        /* Check span tags with color classes */
        if (strncmp(p, "<span class=", 12) == 0 || strncmp(p, "<span class=\\\"", 14) == 0) {
            const char * end_tag = strchr(p, '>');
            if (!end_tag) break;

            char tag_buf[128];
            size_t tlen = (size_t)(end_tag - p + 1);
            if (tlen >= sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
            memcpy(tag_buf, p, tlen);
            tag_buf[tlen] = 0;

            const char * hex_col = NULL;
            if (strstr(tag_buf, "yellow"))      hex_col = "#ffff00 ";
            else if (strstr(tag_buf, "cyan"))   hex_col = "#00ffff ";
            else if (strstr(tag_buf, "green"))  hex_col = "#00ff00 ";
            else if (strstr(tag_buf, "red"))    hex_col = "#ff3333 ";
            else if (strstr(tag_buf, "blue"))   hex_col = "#3388ff ";
            else if (strstr(tag_buf, "white"))  hex_col = "#ffffff ";

            if (hex_col) {
                if (in_color) {
                    out[oi++] = '#'; /* close previous color */
                }
                size_t clen = strlen(hex_col);
                memcpy(out + oi, hex_col, clen);
                oi += clen;
                in_color = 1;
            }
            p = end_tag + 1;
            continue;
        }

        if (strncmp(p, "</span>", 7) == 0) {
            if (in_color) {
                out[oi++] = '#';
                in_color = 0;
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
         * NOS Teletext mosaic characters: &#xF020; to &#xF07F;
         * In Bedstead, the 64 mosaic block graphics are mapped to 0xEE20 to 0xEE7F.
         * Codepoint: 0xEE00 + hex_val.
         * UTF-8 for 0xEE00-0xEE7F: 3 bytes: 0xEE, 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F).
         */
        if (strncmp(p, "&#xF0", 5) == 0 && isxdigit((unsigned char)p[5]) && isxdigit((unsigned char)p[6]) && p[7] == ';') {
            char hex_str[3] = { p[5], p[6], 0 };
            uint32_t byte_val = (uint32_t)strtoul(hex_str, NULL, 16);
            if (byte_val >= 0x20 && byte_val <= 0x7F) {
                uint32_t target_cp = 0xEE00 + byte_val;
                out[oi++] = (char)(0xE0 | (target_cp >> 12));
                out[oi++] = (char)(0x80 | ((target_cp >> 6) & 0x3F));
                out[oi++] = (char)(0x80 | (target_cp & 0x3F));
                p += 8;
                cur_col++;
                continue;
            }
        }

        /* HTML character entities */
        if (strncmp(p, "&quot;", 6) == 0) { out[oi++] = '"'; p += 6; cur_col++; continue; }
        if (strncmp(p, "&amp;", 5) == 0)  { out[oi++] = '&'; p += 5; cur_col++; continue; }
        if (strncmp(p, "&lt;", 4) == 0)   { out[oi++] = '<'; p += 4; cur_col++; continue; }
        if (strncmp(p, "&gt;", 4) == 0)   { out[oi++] = '>'; p += 4; cur_col++; continue; }
        if (strncmp(p, "&apos;", 6) == 0 || strncmp(p, "&#39;", 5) == 0) {
            out[oi++] = '\'';
            p += (strncmp(p, "&#39;", 5) == 0) ? 5 : 6;
            cur_col++;
            continue;
        }

        /* Unescape JSON newlines */
        if (*p == '\\' && *(p + 1) == 'n') {
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

        out[oi++] = *p++;
        cur_col++;
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
    if (s_lbl_header_page) {
        char pbuf[32];
        snprintf(pbuf, sizeof pbuf, "#ffff00 %s#", page);
        lv_label_set_text(s_lbl_header_page, pbuf);
    }

    /* Clear old line-link buttons while loading */
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_link_btns[i]) {
            lv_obj_del(s_link_btns[i]);
            s_link_btns[i] = NULL;
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

    /* Update header page and subpage indicator */
    if (s_lbl_header_page) {
        char pbuf[32];
        snprintf(pbuf, sizeof pbuf, "#ffff00 %s#", s_state.page);
        lv_label_set_text(s_lbl_header_page, pbuf);
    }
    if (s_lbl_sub_info) {
        if (s_state.next_sub[0] || s_state.prev_sub[0]) {
            char sbuf[32];
            const char * cur = strchr(s_state.page, '-');
            snprintf(sbuf, sizeof sbuf, "Deel %s", cur ? cur + 1 : "1");
            lv_label_set_text(s_lbl_sub_info, sbuf);
            lv_obj_clear_flag(s_lbl_sub_info, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_sub_info, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Update text body */
    if (s_lbl_content) {
        lv_label_set_text(s_lbl_content, s_state.text);
    }

    /* Create clickable link overlays for page numbers detected in text */
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_link_btns[i]) {
            lv_obj_del(s_link_btns[i]);
            s_link_btns[i] = NULL;
        }
    }

    if (s_content_box) {
        for (int i = 0; i < s_link_count; i++) {
            int lx = PAD_X + s_links[i].col * CHAR_W - 2;
            int ly = PAD_Y + s_links[i].row * LINE_H - 1;
            int lw = s_links[i].len * CHAR_W + 4;
            int lh = LINE_H + 2;

            lv_obj_t * b = lv_btn_create(s_content_box);
            lv_obj_set_size(b, lw, lh);
            lv_obj_set_pos(b, lx, ly);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0xffd60a), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_radius(b, 2, 0);
            lv_obj_set_style_pad_all(b, 0, 0);

            /* Highlight feedback on press */
            lv_obj_set_style_bg_color(b, lv_color_hex(0xffd60a), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);

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

static void on_quick_cat_click(lv_event_t * e) {
    const char * target = (const char *)lv_event_get_user_data(e);
    if (target) teletekst_request_page(target);
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
    s_lbl_header_page = NULL;
    s_lbl_sub_info = NULL;
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

    /* --- Header Bar --- */
    int header_h = SY(42);
    lv_obj_t * header = lv_obj_create(s_modal);
    lv_obj_set_size(header, LV_HOR_RES, header_h);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x282828), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* Logo & Current Page */
    lv_obj_t * title = lv_label_create(header);
    lv_label_set_recolor(title, true);
    lv_label_set_text(title, "#00ff00 NOS# #ffff00 TT#");
    lv_obj_set_style_text_font(title, SF(17), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, SX(12), 0);

    s_lbl_header_page = lv_label_create(header);
    lv_label_set_recolor(s_lbl_header_page, true);
    lv_label_set_text(s_lbl_header_page, "#ffff00 101#");
    lv_obj_set_style_text_font(s_lbl_header_page, SF(22), 0);
    lv_obj_align(s_lbl_header_page, LV_ALIGN_LEFT_MID, SX(130), 0);

    s_lbl_sub_info = lv_label_create(header);
    lv_obj_set_style_text_color(s_lbl_sub_info, lv_color_hex(0x8e8e93), 0);
    lv_obj_set_style_text_font(s_lbl_sub_info, SF(13), 0);
    lv_label_set_text(s_lbl_sub_info, "");
    lv_obj_align(s_lbl_sub_info, LV_ALIGN_LEFT_MID, SX(182), 0);
    lv_obj_add_flag(s_lbl_sub_info, LV_OBJ_FLAG_HIDDEN);

    /* Quick category pill shortcuts */
    const char * cats[] = {"101", "102", "103", "601", "702", "730"};
    const char * cat_names[] = {"Nieuws", "Binnenl", "Buitenl", "Sport", "Weer", "Verkeer"};
    int cat_x = SX(238);
    for (int i = 0; i < 6; i++) {
        lv_obj_t * b = lv_btn_create(header);
        lv_obj_set_size(b, SX(62), SY(28));
        lv_obj_align(b, LV_ALIGN_LEFT_MID, cat_x + i * SX(66), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x222222), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_quick_cat_click, LV_EVENT_CLICKED, (void*)cats[i]);

        lv_obj_t * lbl = lv_label_create(b);
        lv_label_set_text(lbl, cat_names[i]);
        lv_obj_set_style_text_font(lbl, SF(11), 0);
        lv_obj_center(lbl);
    }

    /* Refresh button */
    lv_obj_t * b_ref = lv_btn_create(header);
    lv_obj_set_size(b_ref, SX(34), SY(28));
    lv_obj_align(b_ref, LV_ALIGN_RIGHT_MID, SX(-46), 0);
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(b_ref, 6, 0);
    lv_obj_add_event_cb(b_ref, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ref = lv_label_create(b_ref);
    lv_label_set_text(lbl_ref, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_ref, SF(14), 0);
    lv_obj_center(lbl_ref);

    /* Close button */
    lv_obj_t * b_close = lv_btn_create(header);
    lv_obj_set_size(b_close, SX(34), SY(28));
    lv_obj_align(b_close, LV_ALIGN_RIGHT_MID, SX(-8), 0);
    lv_obj_set_style_bg_color(b_close, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(b_close, 6, 0);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_close = lv_label_create(b_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_close, SF(14), 0);
    lv_obj_center(lbl_close);

    /* --- Left Panel: CRT Teletext Content Box --- */
    int left_x = SX(12);
    int top_y = header_h + SY(4);

    s_content_box = lv_obj_create(s_modal);
    lv_obj_set_size(s_content_box, content_w, content_h);
    lv_obj_set_pos(s_content_box, left_x, top_y);
    lv_obj_set_style_bg_color(s_content_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(s_content_box, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_border_width(s_content_box, 1, 0);
    lv_obj_set_style_radius(s_content_box, 4, 0);
    lv_obj_set_style_pad_all(s_content_box, PAD_X, 0);
    lv_obj_clear_flag(s_content_box, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_content = lv_label_create(s_content_box);
    lv_label_set_recolor(s_lbl_content, true);
    lv_label_set_long_mode(s_lbl_content, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_lbl_content, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_content, FONT_TELETEKST, 0);
    lv_obj_set_width(s_lbl_content, 40 * CHAR_W + 4);
    lv_obj_align(s_lbl_content, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_content, "#ffd60a Teletekst laden...#");

    /* --- Left Panel: FastText 4 Color Buttons Aligned to Exact Page Width Below --- */
    int ft_y = top_y + content_h + SY(4);
    int ft_h = SY(28);

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
    int panel_h = content_h + ft_h + SY(4);

    lv_obj_t * side_panel = lv_obj_create(s_modal);
    lv_obj_set_size(side_panel, panel_w, panel_h);
    lv_obj_set_pos(side_panel, panel_x, top_y);
    lv_obj_set_style_bg_color(side_panel, lv_color_hex(0x141416), 0);
    lv_obj_set_style_border_color(side_panel, lv_color_hex(0x28282a), 0);
    lv_obj_set_style_border_width(side_panel, 1, 0);
    lv_obj_set_style_radius(side_panel, 8, 0);
    lv_obj_set_style_pad_all(side_panel, SX(10), 0);
    lv_obj_clear_flag(side_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Page Display (top of right panel) */
    lv_obj_t * disp_box = lv_obj_create(side_panel);
    int disp_h = SY(34);
    lv_obj_set_size(disp_box, panel_w - SX(20), disp_h);
    lv_obj_align(disp_box, LV_ALIGN_TOP_MID, 0, 0);
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

    /* 2. Numeric Keypad 3x4 Grid */
    const char * keys[12] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "DEL", "0", "GO"
    };

    int key_gap_x = SX(6);
    int key_gap_y = SY(5);
    int key_w = (panel_w - SX(20) - (2 * key_gap_x)) / 3;
    int key_h = SY(40);
    int key_start_y = disp_h + SY(8);

    for (int i = 0; i < 12; i++) {
        int r = i / 3;
        int c = i % 3;
        lv_obj_t * b = lv_btn_create(side_panel);
        lv_obj_set_size(b, key_w, key_h);
        lv_obj_set_pos(b, c * (key_w + key_gap_x), key_start_y + r * (key_h + key_gap_y));

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
        lv_obj_set_style_text_font(lbl, SF(17), 0);
        lv_obj_center(lbl);
    }

    /* 3. Four Glued Navigation / Scroll Buttons Below Keypad */
    int nav_start_y = key_start_y + 4 * (key_h + key_gap_y) + SY(8);
    int nav_gap = SX(6);
    int nav_w = (panel_w - SX(20) - nav_gap) / 2;
    int nav_h = SY(34);

    /* Row 1: Subpage buttons (< Deel / Deel >) */
    s_btn_prev_sub = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_prev_sub, nav_w, nav_h);
    lv_obj_set_pos(s_btn_prev_sub, 0, nav_start_y);
    lv_obj_set_style_bg_color(s_btn_prev_sub, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_prev_sub, 6, 0);
    lv_obj_set_style_pad_all(s_btn_prev_sub, 0, 0);
    lv_obj_add_event_cb(s_btn_prev_sub, on_prev_sub_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ps = lv_label_create(s_btn_prev_sub);
    lv_label_set_text(lbl_ps, "< Deel");
    lv_obj_set_style_text_font(lbl_ps, SF(13), 0);
    lv_obj_center(lbl_ps);

    s_btn_next_sub = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_next_sub, nav_w, nav_h);
    lv_obj_set_pos(s_btn_next_sub, nav_w + nav_gap, nav_start_y);
    lv_obj_set_style_bg_color(s_btn_next_sub, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_next_sub, 6, 0);
    lv_obj_set_style_pad_all(s_btn_next_sub, 0, 0);
    lv_obj_add_event_cb(s_btn_next_sub, on_next_sub_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ns = lv_label_create(s_btn_next_sub);
    lv_label_set_text(lbl_ns, "Deel >");
    lv_obj_set_style_text_font(lbl_ns, SF(13), 0);
    lv_obj_center(lbl_ns);

    /* Row 2: Page Steppers (<< Vorige / Volgende >>) */
    int page_nav_y = nav_start_y + nav_h + SY(6);

    s_btn_prev_page = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_prev_page, nav_w, nav_h);
    lv_obj_set_pos(s_btn_prev_page, 0, page_nav_y);
    lv_obj_set_style_bg_color(s_btn_prev_page, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_prev_page, 6, 0);
    lv_obj_set_style_pad_all(s_btn_prev_page, 0, 0);
    lv_obj_add_event_cb(s_btn_prev_page, on_prev_page_click, LV_EVENT_CLICKED, NULL);

    s_lbl_prev_page = lv_label_create(s_btn_prev_page);
    lv_label_set_text(s_lbl_prev_page, "<< Vorige");
    lv_obj_set_style_text_font(s_lbl_prev_page, SF(13), 0);
    lv_obj_center(s_lbl_prev_page);

    s_btn_next_page = lv_btn_create(side_panel);
    lv_obj_set_size(s_btn_next_page, nav_w, nav_h);
    lv_obj_set_pos(s_btn_next_page, nav_w + nav_gap, page_nav_y);
    lv_obj_set_style_bg_color(s_btn_next_page, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(s_btn_next_page, 6, 0);
    lv_obj_set_style_pad_all(s_btn_next_page, 0, 0);
    lv_obj_add_event_cb(s_btn_next_page, on_next_page_click, LV_EVENT_CLICKED, NULL);

    s_lbl_next_page = lv_label_create(s_btn_next_page);
    lv_label_set_text(s_lbl_next_page, "Volgende >>");
    lv_obj_set_style_text_font(s_lbl_next_page, SF(13), 0);
    lv_obj_center(s_lbl_next_page);

    /* Timer for async updates (active only during page fetch, paused once loaded) */
    s_ui_timer = lv_timer_create(on_ui_poll, 200, NULL);

    /* Initial fetch: Page 101 */
    teletekst_request_page(s_state.page[0] ? s_state.page : "101");

    return s_modal;
}
