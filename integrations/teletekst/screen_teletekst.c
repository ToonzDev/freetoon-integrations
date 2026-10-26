/*
 * screen_teletekst.c — Full-Screen Interactive NOS Teletekst Viewer for FreeToon
 *
 * Provides authentic Dutch Teletext browsing (pages 100-899, subpages,
 * FastText 4-color shortcuts, quick category presets, and on-screen numeric keypad).
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

#define MAX_CONTENT_RAW  32768
#define MAX_CONTENT_OUT  65536

typedef struct {
    char title[32];
    char page[16];
} fasttext_link_t;

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
static lv_timer_t * s_ui_timer = NULL;

/* Numpad Popup Modal */
static lv_obj_t * s_numpad_modal = NULL;
static lv_obj_t * s_lbl_numpad_digits = NULL;
static char s_entered_digits[8] = "";

/* Forward declarations */
static void teletekst_request_page(const char * page);
static void update_ui_from_state(void);
static void open_numpad(void);
static void close_numpad(void);

/* Clean HTML entities and convert NOS Teletext spans into LVGL recolor tags */
static void parse_teletekst_content(const char * raw_html, char * out, size_t out_max) {
    size_t oi = 0;
    const char * p = raw_html;
    int in_color = 0;

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
                    out[oi++] = '#'; /* close previous */
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

        /* Skip anchor and other HTML tags */
        if (*p == '<') {
            const char * end_tag = strchr(p, '>');
            if (end_tag) {
                p = end_tag + 1;
                continue;
            }
        }

        /* Mosaic / graphic block entities */
        if (strncmp(p, "&#xF020;", 8) == 0) {
            out[oi++] = ' ';
            p += 8;
            continue;
        }
        if (strncmp(p, "&#xF0", 5) == 0) {
            const char * semi = strchr(p, ';');
            if (semi && (semi - p) == 7) {
                /* Common divider lines use &#xF02c; &#xF023; etc. */
                if (strncmp(p, "&#xF07c;", 8) == 0 || strncmp(p, "&#xF07f;", 8) == 0) {
                    out[oi++] = '|';
                } else {
                    out[oi++] = '=';
                }
                p = semi + 1;
                continue;
            }
        }

        /* HTML character entities */
        if (strncmp(p, "&quot;", 6) == 0) { out[oi++] = '"'; p += 6; continue; }
        if (strncmp(p, "&amp;", 5) == 0)  { out[oi++] = '&'; p += 5; continue; }
        if (strncmp(p, "&lt;", 4) == 0)   { out[oi++] = '<'; p += 4; continue; }
        if (strncmp(p, "&gt;", 4) == 0)   { out[oi++] = '>'; p += 4; continue; }
        if (strncmp(p, "&apos;", 6) == 0 || strncmp(p, "&#39;", 5) == 0) {
            out[oi++] = '\'';
            p += (strncmp(p, "&#39;", 5) == 0) ? 5 : 6;
            continue;
        }

        /* Unescape JSON newlines */
        if (*p == '\\' && *(p + 1) == 'n') {
            out[oi++] = '\n';
            p += 2;
            continue;
        }
        if (*p == '\\' && *(p + 1) == 'r') {
            p += 2;
            continue;
        }
        if (*p == '\\' && *(p + 1) == '"') {
            out[oi++] = '"';
            p += 2;
            continue;
        }

        /* Escape literal '#' so LVGL recolor doesn't misinterpret */
        if (*p == '#') {
            out[oi++] = '#';
            out[oi++] = '#';
            p++;
            continue;
        }

        out[oi++] = *p++;
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
        /* Find terminating unescaped quote */
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

static void teletekst_request_page(const char * page) {
    if (!page || !page[0]) return;
    snprintf(s_req_page, sizeof s_req_page, "%s", page);
    s_state.is_loading = 1;
    s_state.has_error = 0;

    if (s_lbl_content) {
        char load_msg[128];
        snprintf(load_msg, sizeof load_msg, "#ffd60a Pagina %s laden...#", page);
        lv_label_set_text(s_lbl_content, load_msg);
    }
    if (s_lbl_header_page) {
        char pbuf[32];
        snprintf(pbuf, sizeof pbuf, "#ffff00 %s#", page);
        lv_label_set_text(s_lbl_header_page, pbuf);
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
        if (s_content_box) {
            lv_obj_scroll_to_y(s_content_box, 0, LV_ANIM_OFF);
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
            lv_label_set_text(s_lbl_prev_page, "<<");
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
            lv_label_set_text(s_lbl_next_page, ">>");
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

/* Numpad Event Handlers */
static void on_numpad_btn(lv_event_t * e) {
    const char * txt = (const char *)lv_event_get_user_data(e);
    if (!txt) return;

    if (strcmp(txt, "DEL") == 0) {
        size_t len = strlen(s_entered_digits);
        if (len > 0) s_entered_digits[len - 1] = 0;
    } else if (strcmp(txt, "GO") == 0) {
        if (s_entered_digits[0]) {
            teletekst_request_page(s_entered_digits);
            close_numpad();
            return;
        }
    } else {
        size_t len = strlen(s_entered_digits);
        if (len < 3) {
            s_entered_digits[len] = txt[0];
            s_entered_digits[len + 1] = 0;
            if (len + 1 == 3) {
                /* Auto-go after 3 digits entered */
                teletekst_request_page(s_entered_digits);
                close_numpad();
                return;
            }
        }
    }

    if (s_lbl_numpad_digits) {
        char disp[16];
        snprintf(disp, sizeof disp, "%s___", s_entered_digits);
        disp[3] = 0;
        lv_label_set_text_fmt(s_lbl_numpad_digits, "Pagina:  #ffd60a %s#", disp);
    }
}

static void close_numpad(void) {
    if (s_numpad_modal) {
        lv_obj_del(s_numpad_modal);
        s_numpad_modal = NULL;
        s_lbl_numpad_digits = NULL;
    }
}

static void on_numpad_close_click(lv_event_t * e) {
    (void)e;
    close_numpad();
}

static void open_numpad(void) {
    if (s_numpad_modal) return;
    s_entered_digits[0] = 0;

    s_numpad_modal = lv_obj_create(s_modal);
    lv_obj_set_size(s_numpad_modal, SX(340), SY(420));
    lv_obj_center(s_numpad_modal);
    lv_obj_set_style_bg_color(s_numpad_modal, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(s_numpad_modal, lv_color_hex(0x3a3a3c), 0);
    lv_obj_set_style_border_width(s_numpad_modal, 2, 0);
    lv_obj_set_style_radius(s_numpad_modal, 12, 0);
    lv_obj_set_style_pad_all(s_numpad_modal, SX(16), 0);
    lv_obj_clear_flag(s_numpad_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Digits display */
    s_lbl_numpad_digits = lv_label_create(s_numpad_modal);
    lv_label_set_recolor(s_lbl_numpad_digits, true);
    lv_obj_set_style_text_font(s_lbl_numpad_digits, SF(22), 0);
    lv_label_set_text(s_lbl_numpad_digits, "Pagina:  #ffd60a ___#");
    lv_obj_align(s_lbl_numpad_digits, LV_ALIGN_TOP_MID, 0, SY(6));

    /* Keypad 3x4 Grid */
    const char * keys[12] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "DEL", "0", "GO"
    };

    int btn_w = SX(86);
    int btn_h = SY(56);
    int start_y = SY(60);

    for (int i = 0; i < 12; i++) {
        int r = i / 3;
        int c = i % 3;
        lv_obj_t * b = lv_btn_create(s_numpad_modal);
        lv_obj_set_size(b, btn_w, btn_h);
        lv_obj_set_pos(b, c * (btn_w + SX(12)), start_y + r * (btn_h + SY(10)));
        if (strcmp(keys[i], "GO") == 0) {
            lv_obj_set_style_bg_color(b, lv_color_hex(0x34c759), 0);
        } else if (strcmp(keys[i], "DEL") == 0) {
            lv_obj_set_style_bg_color(b, lv_color_hex(0xff3b30), 0);
        } else {
            lv_obj_set_style_bg_color(b, lv_color_hex(0x2c2c2e), 0);
        }
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, on_numpad_btn, LV_EVENT_CLICKED, (void*)keys[i]);

        lv_obj_t * lbl = lv_label_create(b);
        lv_label_set_text(lbl, keys[i]);
        lv_obj_set_style_text_font(lbl, SF(20), 0);
        lv_obj_center(lbl);
    }

    /* Cancel button */
    lv_obj_t * b_cancel = lv_btn_create(s_numpad_modal);
    lv_obj_set_size(b_cancel, SX(282), SY(42));
    lv_obj_align(b_cancel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(b_cancel, lv_color_hex(0x3a3a3c), 0);
    lv_obj_set_style_radius(b_cancel, 8, 0);
    lv_obj_add_event_cb(b_cancel, on_numpad_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_c = lv_label_create(b_cancel);
    lv_label_set_text(lbl_c, tr("Annuleren", "Cancel"));
    lv_obj_set_style_text_font(lbl_c, SF(16), 0);
    lv_obj_center(lbl_c);
}

static void on_open_numpad_click(lv_event_t * e) {
    (void)e;
    open_numpad();
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
    s_numpad_modal = NULL;
}

/* Entry point exported to FreeToon core */
lv_obj_t * screen_create(void) {
    if (s_modal) lv_obj_del(s_modal);

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
    lv_obj_t * header = lv_obj_create(s_modal);
    lv_obj_set_size(header, LV_HOR_RES, SY(54));
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
    lv_label_set_text(title, "#00ff00 NOS# #ffff00 TELETEKST#");
    lv_obj_set_style_text_font(title, SF(20), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, SX(16), 0);

    s_lbl_header_page = lv_label_create(header);
    lv_label_set_recolor(s_lbl_header_page, true);
    lv_label_set_text(s_lbl_header_page, "#ffff00 101#");
    lv_obj_set_style_text_font(s_lbl_header_page, SF(24), 0);
    lv_obj_align(s_lbl_header_page, LV_ALIGN_LEFT_MID, SX(210), 0);

    s_lbl_sub_info = lv_label_create(header);
    lv_obj_set_style_text_color(s_lbl_sub_info, lv_color_hex(0x8e8e93), 0);
    lv_obj_set_style_text_font(s_lbl_sub_info, SF(14), 0);
    lv_label_set_text(s_lbl_sub_info, "");
    lv_obj_align(s_lbl_sub_info, LV_ALIGN_LEFT_MID, SX(270), 0);
    lv_obj_add_flag(s_lbl_sub_info, LV_OBJ_FLAG_HIDDEN);

    /* Quick category pill shortcuts */
    const char * cats[] = {"101", "102", "103", "601", "702", "730"};
    const char * cat_names[] = {"Nieuws", "Binnenl", "Buitenl", "Sport", "Weer", "Verkeer"};
    int cat_x = SX(330);
    for (int i = 0; i < 6; i++) {
        lv_obj_t * b = lv_btn_create(header);
        lv_obj_set_size(b, SX(72), SY(34));
        lv_obj_align(b, LV_ALIGN_LEFT_MID, cat_x + i * SX(76), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x222222), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_quick_cat_click, LV_EVENT_CLICKED, (void*)cats[i]);

        lv_obj_t * lbl = lv_label_create(b);
        lv_label_set_text(lbl, cat_names[i]);
        lv_obj_set_style_text_font(lbl, SF(12), 0);
        lv_obj_center(lbl);
    }

    /* Keypad button */
    lv_obj_t * b_numpad = lv_btn_create(header);
    lv_obj_set_size(b_numpad, SX(56), SY(36));
    lv_obj_align(b_numpad, LV_ALIGN_RIGHT_MID, SX(-108), 0);
    lv_obj_set_style_bg_color(b_numpad, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(b_numpad, 8, 0);
    lv_obj_add_event_cb(b_numpad, on_open_numpad_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_np = lv_label_create(b_numpad);
    lv_label_set_text(lbl_np, "123");
    lv_obj_set_style_text_font(lbl_np, SF(15), 0);
    lv_obj_center(lbl_np);

    /* Refresh button */
    lv_obj_t * b_ref = lv_btn_create(header);
    lv_obj_set_size(b_ref, SX(40), SY(36));
    lv_obj_align(b_ref, LV_ALIGN_RIGHT_MID, SX(-60), 0);
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(b_ref, 8, 0);
    lv_obj_add_event_cb(b_ref, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ref = lv_label_create(b_ref);
    lv_label_set_text(lbl_ref, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_ref, SF(16), 0);
    lv_obj_center(lbl_ref);

    /* Close button */
    lv_obj_t * b_close = lv_btn_create(header);
    lv_obj_set_size(b_close, SX(40), SY(36));
    lv_obj_align(b_close, LV_ALIGN_RIGHT_MID, SX(-12), 0);
    lv_obj_set_style_bg_color(b_close, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_radius(b_close, 8, 0);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_close = lv_label_create(b_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_close, SF(16), 0);
    lv_obj_center(lbl_close);

    /* --- Content Body --- */
    s_content_box = lv_obj_create(s_modal);
    lv_obj_set_size(s_content_box, LV_HOR_RES - SX(24), LV_VER_RES - SY(140));
    lv_obj_align(s_content_box, LV_ALIGN_TOP_MID, 0, SY(60));
    lv_obj_set_style_bg_color(s_content_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(s_content_box, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_border_width(s_content_box, 1, 0);
    lv_obj_set_style_radius(s_content_box, 8, 0);
    lv_obj_set_style_pad_all(s_content_box, SX(12), 0);

    s_lbl_content = lv_label_create(s_content_box);
    lv_label_set_recolor(s_lbl_content, true);
    lv_obj_set_style_text_color(s_lbl_content, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_content, SF(16), 0);
    lv_obj_set_width(s_lbl_content, LV_HOR_RES - SX(56));
    lv_label_set_text(s_lbl_content, "#ffd60a Teletekst laden...#");

    /* --- Bottom Toolbar (FastText & Page Stepper) --- */
    lv_obj_t * footer = lv_obj_create(s_modal);
    lv_obj_set_size(footer, LV_HOR_RES, SY(74));
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(0x282828), 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, SX(6), 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 1: FastText 4 Color buttons */
    uint32_t ft_colors[4] = { 0xcc0000, 0x008800, 0xccaa00, 0x0066cc };
    uint32_t ft_text_colors[4] = { 0xffffff, 0xffffff, 0x000000, 0xffffff };
    int ft_w = (LV_HOR_RES - SX(40)) / 4;

    for (int i = 0; i < 4; i++) {
        s_btn_ft[i] = lv_btn_create(footer);
        lv_obj_set_size(s_btn_ft[i], ft_w, SY(32));
        lv_obj_align(s_btn_ft[i], LV_ALIGN_TOP_LEFT, SX(14) + i * (ft_w + SX(4)), SY(2));
        lv_obj_set_style_bg_color(s_btn_ft[i], lv_color_hex(ft_colors[i]), 0);
        lv_obj_set_style_radius(s_btn_ft[i], 6, 0);
        lv_obj_set_style_pad_all(s_btn_ft[i], 0, 0);
        lv_obj_add_event_cb(s_btn_ft[i], on_fasttext_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_flag(s_btn_ft[i], LV_OBJ_FLAG_HIDDEN);

        s_lbl_ft[i] = lv_label_create(s_btn_ft[i]);
        lv_obj_set_style_text_color(s_lbl_ft[i], lv_color_hex(ft_text_colors[i]), 0);
        lv_obj_set_style_text_font(s_lbl_ft[i], SF(13), 0);
        lv_label_set_text(s_lbl_ft[i], "");
        lv_obj_center(s_lbl_ft[i]);
    }

    /* Row 2: Page & Subpage Navigation */
    s_btn_prev_page = lv_btn_create(footer);
    lv_obj_set_size(s_btn_prev_page, SX(130), SY(26));
    lv_obj_align(s_btn_prev_page, LV_ALIGN_BOTTOM_LEFT, SX(14), SY(-2));
    lv_obj_set_style_bg_color(s_btn_prev_page, lv_color_hex(0x242426), 0);
    lv_obj_set_style_radius(s_btn_prev_page, 6, 0);
    lv_obj_add_event_cb(s_btn_prev_page, on_prev_page_click, LV_EVENT_CLICKED, NULL);

    s_lbl_prev_page = lv_label_create(s_btn_prev_page);
    lv_label_set_text(s_lbl_prev_page, "<< Vorige");
    lv_obj_set_style_text_font(s_lbl_prev_page, SF(13), 0);
    lv_obj_center(s_lbl_prev_page);

    s_btn_prev_sub = lv_btn_create(footer);
    lv_obj_set_size(s_btn_prev_sub, SX(100), SY(26));
    lv_obj_align(s_btn_prev_sub, LV_ALIGN_BOTTOM_LEFT, SX(154), SY(-2));
    lv_obj_set_style_bg_color(s_btn_prev_sub, lv_color_hex(0x242426), 0);
    lv_obj_set_style_radius(s_btn_prev_sub, 6, 0);
    lv_obj_add_event_cb(s_btn_prev_sub, on_prev_sub_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ps = lv_label_create(s_btn_prev_sub);
    lv_label_set_text(lbl_ps, "< Deel");
    lv_obj_set_style_text_font(lbl_ps, SF(13), 0);
    lv_obj_center(lbl_ps);

    s_btn_next_sub = lv_btn_create(footer);
    lv_obj_set_size(s_btn_next_sub, SX(100), SY(26));
    lv_obj_align(s_btn_next_sub, LV_ALIGN_BOTTOM_RIGHT, SX(-154), SY(-2));
    lv_obj_set_style_bg_color(s_btn_next_sub, lv_color_hex(0x242426), 0);
    lv_obj_set_style_radius(s_btn_next_sub, 6, 0);
    lv_obj_add_event_cb(s_btn_next_sub, on_next_sub_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ns = lv_label_create(s_btn_next_sub);
    lv_label_set_text(lbl_ns, "Deel >");
    lv_obj_set_style_text_font(lbl_ns, SF(13), 0);
    lv_obj_center(lbl_ns);

    s_btn_next_page = lv_btn_create(footer);
    lv_obj_set_size(s_btn_next_page, SX(130), SY(26));
    lv_obj_align(s_btn_next_page, LV_ALIGN_BOTTOM_RIGHT, SX(-14), SY(-2));
    lv_obj_set_style_bg_color(s_btn_next_page, lv_color_hex(0x242426), 0);
    lv_obj_set_style_radius(s_btn_next_page, 6, 0);
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
