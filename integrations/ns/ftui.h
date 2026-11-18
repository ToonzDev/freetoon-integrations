/*
 * ftui.h - small LVGL kit for FreeToon UI plugins (header-only).
 *
 * A calm look: an 8 px grid, no outlines, depth from a quiet step in surface
 * colour (THEME_BG -> THEME_CARD_BG -> THEME_ROW_BG), one accent
 * (THEME_ACCENT), big numbers in Open Sans Light, small letter-spaced
 * captions. Colours come only from the THEME_* tokens (theme.h), sizes from
 * SX/SY/SF so Toon 1 (800x480) works too.
 *
 *   ftui_header()            back button, caption and title of a screen
 *   ftui_card / ftui_caption / ftui_label / ftui_light (light numerals)
 *   ftui_button / ftui_icon_button / ftui_ring / ftui_bar / ftui_scroller
 *   ftui_prompt()            text field + on-screen keyboard in a panel
 *   ftui_field_row()         tappable settings row (caption, value, chevron)
 *   ftui_conf_get/set()      one key=value line of the app's config file
 *   ftui_cmd()               hand a JSON command to the daemon
 *
 * Define FTUI_ID ("bambuddy", ...) before including. Identical copy in every
 * integration folder that uses it.
 */
#ifndef FTUI_H
#define FTUI_H

#ifndef FTUI_ID
#error "define FTUI_ID before including ftui.h"
#endif

#include "lvgl/lvgl.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "screens.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef FTUI_DIR
#define FTUI_DIR "/mnt/data/integrations/" FTUI_ID
#endif
#ifndef FTUI_RUN
#define FTUI_RUN "/var/volatile/tmp/" FTUI_ID
#endif

/* 8 px grid (design px, scaled on Toon 1) */
#define FTUI_G(n)    SX(8 * (n))
#define FTUI_GY(n)   SY(8 * (n))
#define FTUI_MARGIN  FTUI_G(4)          /* 32: screen edge */
#define FTUI_GAP     FTUI_G(2)          /* 16: between cards */
#define FTUI_HEAD_Y  FTUI_GY(3)         /* 24 */
#define FTUI_TOP     FTUI_GY(14)        /* 112: content starts below the header */
#define FTUI_BTN     SX(56)             /* minimum touch target */

/* Open Sans Light readout fonts (digits, : % ° + - . /) live in toonui. Weak,
 * so a toonui without them still loads the plugin (Montserrat then). */
extern const lv_font_t lv_font_os_light_30 __attribute__((weak));
extern const lv_font_t lv_font_os_light_40 __attribute__((weak));
extern const lv_font_t lv_font_os_light_50 __attribute__((weak));
extern const lv_font_t lv_font_os_light_90 __attribute__((weak));

static const lv_font_t * ftui_light(int design_px) {
    int p = (design_px * DISP_HOR) / DESIGN_HOR;
    const lv_font_t * f = NULL;
    if (p >= 70) f = &lv_font_os_light_90;
    else if (p >= 45) f = &lv_font_os_light_50;
    else if (p >= 35) f = &lv_font_os_light_40;
    else f = &lv_font_os_light_30;
    return f ? f : SF(design_px);
}

/* ---- files ---------------------------------------------------------------- */

static char * ftui_read(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (4 << 20)) { fclose(f); return NULL; }
    char * b = (char *)malloc((size_t)n + 1);
    if (b) { size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; }
    fclose(f);
    return b;
}

static int ftui_write_atomic(const char * path, const char * text) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    const char * p = text;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        p += w;
        len -= (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Value of key in a key=value file ("" if missing). */
static void ftui_conf_get(const char * path, const char * key, char * out, size_t n) {
    out[0] = 0;
    char * t = ftui_read(path);
    if (!t) return;
    size_t kl = strlen(key);
    for (char * line = t; line && *line; ) {
        char * eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        if (ll > kl && !strncmp(line, key, kl) && line[kl] == '=') {
            size_t vl = ll - kl - 1;
            while (vl > 0 && (line[kl + vl] == '\r' || line[kl + vl] == ' ')) vl--;
            if (vl >= n) vl = n - 1;
            memcpy(out, line + kl + 1, vl);
            out[vl] = 0;
        }
        line = eol ? eol + 1 : NULL;
    }
    free(t);
}

/* Set key=value in the file, keeping every other line (and comments). */
static int ftui_conf_set(const char * path, const char * key, const char * value) {
    /* config is line based: a line break in a value would add settings */
    for (const unsigned char * p = (const unsigned char *)value; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return -1;
    char * t = ftui_read(path);
    size_t kl = strlen(key), cap = (t ? strlen(t) : 0) + strlen(value) + kl + 8;
    char * out = (char *)malloc(cap);
    if (!out) { free(t); return -1; }
    size_t o = 0;
    int done = 0;
    for (char * line = t; line && *line; ) {
        char * eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        if (ll >= kl + 1 && !strncmp(line, key, kl) && line[kl] == '=') {
            if (!done) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
            done = 1;
        } else {
            memcpy(out + o, line, ll);
            o += ll;
            out[o++] = '\n';
        }
        line = eol ? eol + 1 : NULL;
    }
    if (!done) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
    out[o] = 0;
    int rc = ftui_write_atomic(path, out);
    free(out);
    free(t);
    return rc;
}

/* Drop a command for the daemon; it polls FTUI_RUN twice a second. */
static void ftui_cmd(const char * json) {
    static unsigned seq;
    char path[200];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    mkdir(FTUI_RUN, 0755);
    snprintf(path, sizeof path, FTUI_RUN "/cmd-%010ld%03ld-%04u.json", (long)ts.tv_sec, ts.tv_nsec / 1000000,
             seq++ % 10000);
    ftui_write_atomic(path, json);
}

/* ---- widgets -------------------------------------------------------------- */

static lv_obj_t * ftui_plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t * ftui_card(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, uint32_t bg) {
    lv_obj_t * c = ftui_plain(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, THEME_RADIUS_TILE, 0);
    return c;
}

static lv_obj_t * ftui_label(lv_obj_t * parent, const char * text, const lv_font_t * font, uint32_t color) {
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t * ftui_label_at(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, const char * text,
                                const lv_font_t * font, uint32_t color) {
    lv_obj_t * l = ftui_label(parent, text, font, color);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* Small upper-case label with a little letter spacing ("NOZZLE", "SPOOR"). */
static lv_obj_t * ftui_caption(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, const char * text) {
    char up[160];
    size_t i = 0;
    for (; text && text[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)text[i]);
    up[i] = 0;
    lv_obj_t * l = ftui_label_at(parent, x, y, up, SF(12), THEME_TEXT_MUTED);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

/* Big light numerals ("2:14", "63%", "20°"). */
static lv_obj_t * ftui_number(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, const char * text, int design_px,
                              uint32_t color) {
    return ftui_label_at(parent, x, y, text, ftui_light(design_px), color);
}

/* Single line cut off with "..." (LONG_DOT needs a fixed height in LVGL 8). */
static void ftui_one_line(lv_obj_t * l, lv_coord_t w) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

static lv_obj_t * ftui_button(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                              uint32_t bg, uint32_t fg, const char * text, const lv_font_t * font,
                              lv_event_cb_t cb, intptr_t data) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, THEME_RADIUS_BTN, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, 4);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)data);
    if (text) {
        lv_obj_t * l = ftui_label(b, text, font, fg);
        lv_obj_center(l);
    }
    return b;
}

/* Round ghost button with a symbol (back, settings, refresh). */
static lv_obj_t * ftui_icon_button(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, const char * symbol,
                                   lv_event_cb_t cb, intptr_t data) {
    lv_obj_t * b = ftui_button(parent, x, y, FTUI_BTN, FTUI_BTN, THEME_CARD_BG, THEME_TEXT_DIM, symbol, SF(20), cb, data);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    return b;
}

/* Screen header: back (round), caption above a title. Returns the title label. */
static lv_obj_t * ftui_header(lv_obj_t * parent, const char * caption, const char * title, lv_event_cb_t on_back) {
    ftui_icon_button(parent, FTUI_MARGIN, FTUI_HEAD_Y, LV_SYMBOL_LEFT, on_back, 0);
    lv_coord_t tx = FTUI_MARGIN + FTUI_BTN + FTUI_G(2);
    ftui_caption(parent, tx, FTUI_HEAD_Y + SY(2), caption);
    lv_obj_t * t = ftui_label_at(parent, tx, FTUI_HEAD_Y + SY(20), title, SF(28), THEME_TEXT_HI);
    ftui_one_line(t, SX(560));
    return t;
}

/* Thin progress ring (0..1000 permille). */
static lv_obj_t * ftui_ring(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, int permille,
                            uint32_t color, lv_coord_t width) {
    lv_obj_t * a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_pos(a, x, y);
    lv_obj_set_size(a, size, size);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 1000);
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    lv_arc_set_value(a, (int16_t)permille);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(THEME_ROW_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, 1, LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

/* Thin horizontal bar, 0..1000 permille. */
static void ftui_bar(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                     int permille, uint32_t fill) {
    lv_obj_t * track = ftui_plain(parent);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, w, h);
    lv_obj_set_style_bg_color(track, lv_color_hex(THEME_ROW_BG), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, h / 2, 0);
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    lv_coord_t fw = (lv_coord_t)((int)w * permille / 1000);
    if (fw > 0 && fw < h) fw = h;
    if (fw <= 0) return;
    lv_obj_t * f = ftui_plain(track);
    lv_obj_set_size(f, fw, h);
    lv_obj_set_style_bg_color(f, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f, h / 2, 0);
}

static lv_obj_t * ftui_dot(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t d, uint32_t color) {
    lv_obj_t * o = ftui_card(parent, x, y, d, d, color);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}

static lv_obj_t * ftui_scroller(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t * s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s, lv_color_hex(THEME_TEXT_MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s, 2, LV_PART_SCROLLBAR);
    return s;
}

/* ---- text prompt (panel with keyboard) ------------------------------------ */

typedef void (*ftui_prompt_cb)(const char * key, const char * value);

static lv_obj_t * ftui_modal_;
static lv_obj_t * ftui_prompt_ta_;
static ftui_prompt_cb ftui_prompt_done_;
static char ftui_prompt_key_[32];

static void ftui_modal_close(void) {
    if (ftui_modal_) { lv_obj_del_async(ftui_modal_); ftui_modal_ = NULL; ftui_prompt_ta_ = NULL; }
}

static int ftui_modal_open_(void) { return ftui_modal_ != NULL; }

static lv_obj_t * ftui_modal_new(lv_obj_t * root) {
    if (ftui_modal_) lv_obj_del(ftui_modal_);
    ftui_modal_ = ftui_plain(root);
    lv_obj_set_size(ftui_modal_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ftui_modal_, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(ftui_modal_, LV_OPA_COVER, 0);
    lv_obj_add_flag(ftui_modal_, LV_OBJ_FLAG_CLICKABLE);
    return ftui_modal_;
}

static void ftui_prompt_save_(void) {
    if (!ftui_prompt_ta_) return;
    static char val[1100];                         /* tokens can be long (JWT) */
    snprintf(val, sizeof val, "%s", lv_textarea_get_text(ftui_prompt_ta_));
    char * s = val;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
    for (char * p = s; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';   /* config is line based */
    ftui_prompt_cb cb = ftui_prompt_done_;
    char key[32];
    snprintf(key, sizeof key, "%s", ftui_prompt_key_);
    ftui_modal_close();
    if (cb) cb(key, s);
}

static void ftui_prompt_on_save_(lv_event_t * e) { (void)e; ftui_prompt_save_(); }
static void ftui_prompt_on_cancel_(lv_event_t * e) { (void)e; ftui_modal_close(); }
static void ftui_prompt_on_kb_(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) ftui_prompt_save_();
    else if (code == LV_EVENT_CANCEL) ftui_modal_close();
}

static void ftui_prompt(lv_obj_t * root, const char * key, const char * title, const char * hint,
                        const char * initial, int max_len, ftui_prompt_cb done) {
    lv_obj_t * m = ftui_modal_new(root);
    snprintf(ftui_prompt_key_, sizeof ftui_prompt_key_, "%s", key);
    ftui_prompt_done_ = done;

    lv_coord_t w = SX(1024) - 2 * FTUI_MARGIN;
    ftui_caption(m, FTUI_MARGIN, FTUI_GY(3), title);
    lv_obj_t * h = ftui_label_at(m, FTUI_MARGIN, FTUI_GY(5), hint, SF(14), THEME_TEXT_DIM);
    ftui_one_line(h, w);

    ftui_prompt_ta_ = lv_textarea_create(m);
    lv_obj_remove_style_all(ftui_prompt_ta_);
    lv_textarea_set_one_line(ftui_prompt_ta_, true);
    lv_textarea_set_max_length(ftui_prompt_ta_, (uint32_t)(max_len > 0 && max_len < 1090 ? max_len : 1090));
    lv_textarea_set_text(ftui_prompt_ta_, initial ? initial : "");
    lv_obj_set_pos(ftui_prompt_ta_, FTUI_MARGIN, FTUI_GY(9));
    lv_obj_set_size(ftui_prompt_ta_, w - SX(2 * 136 + 2 * 16), FTUI_BTN);
    lv_obj_set_style_text_font(ftui_prompt_ta_, SF(22), 0);
    lv_obj_set_style_text_color(ftui_prompt_ta_, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_bg_color(ftui_prompt_ta_, lv_color_hex(THEME_INPUT_BG), 0);
    lv_obj_set_style_bg_opa(ftui_prompt_ta_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ftui_prompt_ta_, THEME_RADIUS_BTN, 0);
    lv_obj_set_style_pad_hor(ftui_prompt_ta_, SX(16), 0);
    lv_obj_set_style_pad_ver(ftui_prompt_ta_, SY(14), 0);
    lv_obj_set_style_bg_color(ftui_prompt_ta_, lv_color_hex(THEME_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ftui_prompt_ta_, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(ftui_prompt_ta_, 2, LV_PART_CURSOR);
    lv_obj_add_state(ftui_prompt_ta_, LV_STATE_FOCUSED);

    lv_coord_t bx = SX(1024) - FTUI_MARGIN - SX(136);
    ftui_button(m, bx, FTUI_GY(9), SX(136), FTUI_BTN, THEME_ACCENT_FILL, THEME_ON_ACCENT,
                tr("Opslaan", "Save"), SF(18), ftui_prompt_on_save_, 0);
    ftui_button(m, bx - SX(152), FTUI_GY(9), SX(136), FTUI_BTN, THEME_CARD_BG, THEME_TEXT_HI,
                tr("Annuleer", "Cancel"), SF(18), ftui_prompt_on_cancel_, 0);

    lv_obj_t * kb = lv_keyboard_create(m);
    lv_obj_set_size(kb, LV_HOR_RES, SY(352));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, SF(22), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, SX(8), 0);
    lv_obj_set_style_pad_gap(kb, SX(8), 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(THEME_TEXT_HI), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, THEME_RADIUS_SMALL, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_ROW_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(THEME_TEXT_HI), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_BTN_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_keyboard_set_textarea(kb, ftui_prompt_ta_);
    lv_obj_add_event_cb(kb, ftui_prompt_on_kb_, LV_EVENT_ALL, NULL);
}

/* ---- settings rows --------------------------------------------------------- */

typedef struct {
    const char * key;        /* config key */
    const char * label_nl, * label_en;
    const char * hint_nl, * hint_en;
    int          secret;     /* show only the last 4 characters */
    int          max_len;
} ftui_field_t;

/* Tappable row: caption, current value, chevron. Returns the row height. */
static lv_coord_t ftui_field_row(lv_obj_t * parent, lv_coord_t y, lv_coord_t w, const ftui_field_t * f,
                                 const char * conf_path, lv_event_cb_t on_edit, intptr_t idx) {
    lv_coord_t h = FTUI_GY(10);
    lv_obj_t * row = ftui_button(parent, 0, y, w, h, THEME_CARD_BG, THEME_TEXT_HI, NULL, NULL, on_edit, idx);
    lv_obj_set_style_radius(row, THEME_RADIUS_TILE, 0);
    ftui_caption(row, FTUI_G(3), FTUI_GY(2), tr(f->label_nl, f->label_en));
    char val[200], shown[200];
    ftui_conf_get(conf_path, f->key, val, sizeof val);
    if (!val[0]) snprintf(shown, sizeof shown, "%s", tr("Niet ingesteld", "Not set"));
    else if (f->secret) {
        size_t n = strlen(val);
        snprintf(shown, sizeof shown, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 %s", n > 4 ? val + n - 4 : "");
    } else snprintf(shown, sizeof shown, "%s", val);
    lv_obj_t * v = ftui_label_at(row, FTUI_G(3), FTUI_GY(4) + SY(4), shown, SF(20), val[0] ? THEME_TEXT_HI : THEME_TEXT_MUTED);
    ftui_one_line(v, w - FTUI_G(12));
    lv_obj_t * ch = ftui_label(row, LV_SYMBOL_RIGHT, SF(18), THEME_TEXT_MUTED);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -FTUI_G(3), 0);
    return h;
}

/* Row with a caption, a one-line explanation and a switch for a 0/1 config
 * key (missing = 0, so every option starts off). Returns the row height. */
static void ftui_toggle_cb_(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    const char * key = (const char *)lv_event_get_user_data(e);
    const char * path = (const char *)lv_obj_get_user_data(sw);
    if (key && path) ftui_conf_set(path, key, lv_obj_has_state(sw, LV_STATE_CHECKED) ? "1" : "0");
    ftui_cmd("{\"cmd\":\"refresh\"}");
}

static lv_coord_t ftui_toggle_row(lv_obj_t * parent, lv_coord_t y, lv_coord_t w, const char * key,
                                  const char * label, const char * hint, const char * conf_path) {
    lv_coord_t h = FTUI_GY(10);
    lv_obj_t * row = ftui_card(parent, 0, y, w, h, THEME_CARD_BG);
    ftui_caption(row, FTUI_G(3), FTUI_GY(2), label);
    lv_obj_t * hl = ftui_label_at(row, FTUI_G(3), FTUI_GY(4) + SY(6), hint, SF(14), THEME_TEXT_DIM);
    ftui_one_line(hl, w - FTUI_G(14));
    char v[8];
    ftui_conf_get(conf_path, key, v, sizeof v);
    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_set_size(sw, SX(56), SY(32));
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -FTUI_G(3), 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_ACCENT_FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_ON_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -SX(3), LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, SX(16));
    if (atoi(v) == 1) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_user_data(sw, (void *)conf_path);           /* string literals: live forever */
    lv_obj_add_event_cb(sw, ftui_toggle_cb_, LV_EVENT_VALUE_CHANGED, (void *)key);
    return h;
}

/* Section caption inside a settings list. Returns its height. */
static lv_coord_t ftui_section(lv_obj_t * parent, lv_coord_t y, const char * text) {
    ftui_caption(parent, FTUI_G(1), y + FTUI_GY(2), text);
    return FTUI_GY(5);
}

/* "12 s" / "3 min" / "2 uur" ago */
static void ftui_ago(long secs, char * out, size_t n) {
    if (secs < 0) { snprintf(out, n, "%s", tr("nooit", "never")); return; }
    if (secs < 60) snprintf(out, n, tr("%ld s geleden", "%ld s ago"), secs);
    else if (secs < 3600) snprintf(out, n, tr("%ld min geleden", "%ld min ago"), secs / 60);
    else snprintf(out, n, tr("%ld uur geleden", "%ld h ago"), secs / 3600);
}

#endif /* FTUI_H */
