/*
 * screen_radiatoren.c - FreeToon UI plugin (radiatoren_ui.<toon>.so) for the
 * Radiatoren app: rooms with their own comfort/eco temperature and weekly
 * time blocks. toonui dlopen()s this when the Radiatoren tile is tapped and
 * calls screen_create().
 *
 * Two views on one screen:
 *   - overview: a card per room (name, target now, status/next switch,
 *     -/+ quick adjust, mode, settings), "+ Kamer" to add one;
 *   - editor:   name (keyboard), comfort/eco, mode, target now, a week
 *     overview and up to four time blocks; delete with confirmation.
 *
 * Every edit is written straight away (atomically) to the settings file the
 * QML app used, /mnt/data/tsc/radiatoren.userSettings.json. The daemon sees
 * the new file within a second and publishes /qmf/www/tsc/radiatoren.json
 * for Home Assistant. Valves are linked in Home Assistant: a radiator valve
 * follows the room whose name equals its Home Assistant area.
 */
#define _GNU_SOURCE
/* Layout independence: every `settings` below (also inside FreeToon's inline
 * helpers such as tr() and the THEME_* fallbacks) is this plugin's own copy,
 * filled from toonui.cfg in host_settings_refresh(). The plugin never reads
 * toonui's settings_t, so it declares no settings fields and keeps loading
 * when FreeToon moves fields around (1.0.12 -> 1.0.13 moved home_light_mode
 * twice, and every toonui then refused the plugin). */
#define settings rad_host_settings
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "plugin_abi.h"
#include "radiatoren_logic.h"
#include "icon_radiator.h"     /* FreeToon's own radiator glyph (copied in, hidden) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* No settings fields: see host_settings_refresh(). */
FT_PLUGIN_DECLARE_ABI_NO_SETTINGS();

settings_t rad_host_settings;   /* language + home_light_mode from toonui.cfg */

/* The two values the plugin needs, read from toonui's own config file (toonui
 * writes it on every settings change). Missing file or key: keep what is
 * there (the host preview sets the fields itself). */
static void host_settings_refresh(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    char path[256], line[160];
    snprintf(path, sizeof path, "%s/toonui.cfg", d && d[0] ? d : "/mnt/data");
    FILE * f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "language=", 9))
            settings.language = atoi(line + 9) == LANG_EN ? LANG_EN : LANG_NL;
        else if (!strncmp(line, "home_light_mode=", 16))
            settings.home_light_mode = atoi(line + 16) ? 1 : 0;
    }
    fclose(f);
}

#define EXPORT __attribute__((visibility("default")))

/* ---- colours (FreeToon theme + the schedule screen's palette) ----------- */
#define COL_COMFORT   0xff8866                                       /* schedule "Comfort" */
#define COL_ECO       (settings.home_light_mode ? 0x2563eb : 0x4f8fe0)
#define COL_MANUAL    (settings.home_light_mode ? 0x7c3aed : 0x9a5cf0)
#define COL_ACCENT    (settings.home_light_mode ? 0x2563eb : 0x3388aa) /* schedule "selected"/"save" */
#define COL_DANGER    0xaa3344
#define COL_TEMP      THEME_WARN_ORANGE                              /* home screen temperature */

/* ---- geometry (Toon 2 design space; SX/SY keep Toon 1 workable) ---------- */
#define PAD           SX(16)
#define HEAD_H        SY(56)
#define BTN           SX(56)           /* minimum touch target */
#define CARD_R        14

/* ---- state ---------------------------------------------------------------- */
enum { VIEW_OVERVIEW = 0, VIEW_EDITOR = 1 };

static rad_settings_t s_set;
static int        s_view = VIEW_OVERVIEW;
static char       s_edit_id[RAD_ID_BYTES];
static int        s_save_failed;

static lv_obj_t * s_root;
static lv_obj_t * s_body;              /* rebuilt for every change */
static lv_obj_t * s_scroll;            /* the scrolling list in the current view */
static lv_obj_t * s_modal;             /* keyboard / delete confirmation */
static lv_obj_t * s_name_ta;
static lv_timer_t * s_timer;
static int        s_rebuild_pending;
static lv_coord_t s_keep_scroll = -1;

static void rebuild(void);
static void request_rebuild(void);

/* ======================================================================== */
/* helpers                                                                  */
/* ======================================================================== */

static const char * settings_path(void) {
    const char * e = getenv("RADIATOREN_SETTINGS");     /* host preview only */
    return e && e[0] ? e : RAD_SETTINGS_PATH;
}

static struct tm now_tm(void) {
    time_t t = time(NULL);
#ifdef RAD_HOST_PREVIEW
    const char * fake = getenv("RADIATOREN_NOW");       /* fixed clock for screenshots */
    if (fake && fake[0]) t = (time_t)atoll(fake);
#endif
    struct tm tm;
    localtime_r(&t, &tm);
    return tm;
}

static void fmt_temp(double v, char * out, size_t n) {
    rad_format_temp(v, out, n);
    if (settings.language == LANG_EN) { char * c = strchr(out, ','); if (c) *c = '.'; }
}

/* Monday-first short day name. i18n_day_short() takes tm_wday (Sunday = 0). */
static const char * day_short(int monday_index) { return i18n_day_short((monday_index + 1) % 7); }

static uint32_t reason_color(rad_reason_t r) {
    switch (r) {
    case RAD_REASON_COMFORT: return COL_COMFORT;
    case RAD_REASON_ECO:     return COL_ECO;
    case RAD_REASON_MANUAL:  return COL_MANUAL;
    default:                 return THEME_OFF;
    }
}

static uint32_t mode_color(rad_mode_t m) {
    return m == RAD_MODE_AUTO ? COL_ACCENT : m == RAD_MODE_MANUAL ? COL_MANUAL : THEME_OFF;
}

static const char * mode_label(rad_mode_t m) {
    return m == RAD_MODE_AUTO ? tr("Schema", "Schedule")
         : m == RAD_MODE_MANUAL ? tr("Handmatig", "Manual") : tr("Uit", "Off");
}

static void status_text(const rad_room_t * r, const struct tm * tm, char * out, size_t n) {
    rad_target_t t = rad_target_for(r, tm);
    if (t.reason == RAD_REASON_OFF) { snprintf(out, n, "%s", tr("Uit, vorstvrij (6\xc2\xb0)", "Off, frost protection (6\xc2\xb0)")); return; }
    if (t.reason == RAD_REASON_MANUAL) { snprintf(out, n, "%s", tr("Handmatig, schema staat stil", "Manual, schedule paused")); return; }
    const char * label = t.reason == RAD_REASON_COMFORT ? "Comfort" : "Eco";
    if (r->n_blocks == 0) { snprintf(out, n, tr("%s, nog geen tijdblokken", "%s, no time blocks yet"), label); return; }
    int next = rad_next_change_today(r, tm);
    if (next < 0) { snprintf(out, n, tr("%s, vandaag geen wissel meer", "%s, no more changes today"), label); return; }
    char hm[8];
    rad_format_minutes(next, hm, sizeof hm);
    snprintf(out, n, tr("%s tot %s", "%s until %s"), label, hm);
}

/* Persist and redraw. Called after every successful edit. */
static void commit(void) {
    rad_normalize_settings(&s_set);
    s_save_failed = rad_save_settings(&s_set, settings_path()) != 0;
    if (s_save_failed) fprintf(stderr, "[radiatoren] saving %s failed\n", settings_path());
    request_rebuild();
}

static int edit_index(void) { return rad_room_index(&s_set, s_edit_id); }

/* ---- widget factories ---------------------------------------------------- */

static lv_obj_t * plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t * card(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, uint32_t bg) {
    lv_obj_t * c = plain(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, CARD_R, 0);
    return c;
}

static lv_obj_t * label(lv_obj_t * parent, const char * text, const lv_font_t * font, uint32_t color) {
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t * button(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                         uint32_t bg, uint32_t fg, const char * text, const lv_font_t * font,
                         lv_event_cb_t cb, intptr_t data) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 4);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)data);
    if (text) {
        lv_obj_t * l = label(b, text, font, fg);
        lv_obj_center(l);
    }
    return b;
}

/* Single line, cut off with "..." (LONG_DOT needs a fixed height in LVGL 8). */
static void one_line(lv_obj_t * l, lv_coord_t w) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

static void divider(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t * d = plain(parent);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_size(d, w, 1);
    lv_obj_set_style_bg_color(d, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_60, 0);
}

static lv_obj_t * scroller(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t * s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s, lv_color_hex(THEME_BORDER), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s, 3, LV_PART_SCROLLBAR);
    return s;
}

/* Back button in the style of the other FreeToon screens. */
static lv_obj_t * back_button(lv_obj_t * parent, lv_event_cb_t cb) {
    char txt[48];
    snprintf(txt, sizeof txt, LV_SYMBOL_LEFT "  %s", tr("Terug", "Back"));
    return button(parent, PAD, SY(12), SX(132), HEAD_H, THEME_BTN_BG, THEME_TEXT_HI, txt, SF(20), cb, 0);
}

/* ======================================================================== */
/* modal: rename (keyboard) and delete confirmation                         */
/* ======================================================================== */

static void modal_close(void) {
    if (s_modal) { lv_obj_del_async(s_modal); s_modal = NULL; s_name_ta = NULL; }
    request_rebuild();
}

static lv_obj_t * modal_open(void) {
    if (s_modal) lv_obj_del(s_modal);
    s_modal = plain(s_root);
    lv_obj_set_size(s_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_60, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);     /* swallow taps behind it */
    return s_modal;
}

static void on_modal_cancel(lv_event_t * e) { (void)e; modal_close(); }

static void save_name(void) {
    if (!s_name_ta) return;
    if (rad_rename_room(&s_set, edit_index(), lv_textarea_get_text(s_name_ta))) commit();
    modal_close();
}

static void on_name_save(lv_event_t * e) { (void)e; save_name(); }

static void on_name_kb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) save_name();
    else if (code == LV_EVENT_CANCEL) modal_close();
}

static void open_rename(lv_event_t * e) {
    (void)e;
    int idx = edit_index();
    if (idx < 0) return;
    lv_obj_t * m = modal_open();

    lv_obj_t * c = card(m, SX(112), SY(24), SX(800), SY(200), THEME_CARD_BG);
    lv_obj_t * t = label(c, tr("Naam van de kamer", "Room name"), SF(22), THEME_TEXT_HI);
    lv_obj_set_pos(t, SX(24), SY(18));
    lv_obj_t * hint = label(c, tr("Gebruik dezelfde naam als de ruimte in Home Assistant.",
                                  "Use the same name as the area in Home Assistant."), SF(14), THEME_TEXT_DIM);
    lv_obj_set_pos(hint, SX(24), SY(50));

    s_name_ta = lv_textarea_create(c);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, RAD_NAME_CHARS);
    lv_textarea_set_text(s_name_ta, s_set.rooms[idx].name);
    lv_obj_set_pos(s_name_ta, SX(24), SY(84));
    lv_obj_set_size(s_name_ta, SX(752), SY(52));
    lv_obj_set_style_text_font(s_name_ta, SF(22), 0);
    lv_obj_set_style_bg_color(s_name_ta, lv_color_hex(THEME_ROW_BG), 0);
    lv_obj_set_style_text_color(s_name_ta, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_border_color(s_name_ta, lv_color_hex(COL_ACCENT), 0);
    lv_obj_add_state(s_name_ta, LV_STATE_FOCUSED);

    button(c, SX(456), SY(144), SX(150), SY(48), THEME_BTN_BG, THEME_TEXT_HI,
           tr("Annuleer", "Cancel"), SF(18), on_modal_cancel, 0);
    button(c, SX(626), SY(144), SX(150), SY(48), COL_ACCENT, 0xffffff,
           tr("Opslaan", "Save"), SF(18), on_name_save, 0);

    lv_obj_t * kb = lv_keyboard_create(m);
    lv_obj_set_size(kb, LV_HOR_RES, SY(330));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, SF(22), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_CARD_BG), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(THEME_TEXT_HI), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_BTN_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(THEME_TEXT_HI), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(THEME_BTN_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_keyboard_set_textarea(kb, s_name_ta);
    lv_obj_add_event_cb(kb, on_name_kb, LV_EVENT_ALL, NULL);
}

static void on_delete_confirm(lv_event_t * e) {
    (void)e;
    if (rad_remove_room(&s_set, edit_index())) {
        s_view = VIEW_OVERVIEW;
        s_keep_scroll = 0;
        commit();
    }
    modal_close();
}

static void open_delete(lv_event_t * e) {
    (void)e;
    int idx = edit_index();
    if (idx < 0) return;
    lv_obj_t * m = modal_open();
    lv_obj_add_event_cb(m, on_modal_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t * c = card(m, 0, 0, SX(600), SY(232), THEME_CARD_BG);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(c);
    char txt[160];
    snprintf(txt, sizeof txt, tr("\"%s\" verwijderen?", "Delete \"%s\"?"), s_set.rooms[idx].name);
    lv_obj_t * t = label(c, txt, SF(22), THEME_TEXT_HI);
    lv_obj_set_pos(t, SX(28), SY(26));
    lv_obj_t * w = label(c, tr("De kamer en haar tijdblokken verdwijnen. Home Assistant krijgt voor deze "
                               "kamer geen doeltemperatuur meer.",
                               "The room and its time blocks are removed. Home Assistant no longer gets "
                               "a target temperature for this room."), SF(18), THEME_TEXT_DIM);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, SX(544));
    lv_obj_set_pos(w, SX(28), SY(70));

    button(c, SX(200), SY(152), SX(170), BTN, THEME_BTN_BG, THEME_TEXT_HI,
           tr("Annuleer", "Cancel"), SF(20), on_modal_cancel, 0);
    char del[48];
    snprintf(del, sizeof del, LV_SYMBOL_TRASH "  %s", tr("Verwijderen", "Delete"));
    button(c, SX(390), SY(152), SX(182), BTN, COL_DANGER, 0xffffff, del, SF(20), on_delete_confirm, 0);
}

/* ======================================================================== */
/* overview                                                                 */
/* ======================================================================== */

static void on_close(lv_event_t * e) { (void)e; ui_pop(); }

static void open_editor(const char * id) {
    snprintf(s_edit_id, sizeof s_edit_id, "%s", id);
    s_view = VIEW_EDITOR;
    s_keep_scroll = 0;
    request_rebuild();
}

static void on_add_room(lv_event_t * e) {
    (void)e;
    char id[RAD_ID_BYTES];
    if (!rad_add_room(&s_set, id, sizeof id)) return;
    commit();
    open_editor(id);
}

static void on_card_minus(lv_event_t * e) {
    struct tm tm = now_tm();
    if (rad_nudge_target(&s_set, (int)(intptr_t)lv_event_get_user_data(e), -RAD_STEP, &tm)) commit();
}
static void on_card_plus(lv_event_t * e) {
    struct tm tm = now_tm();
    if (rad_nudge_target(&s_set, (int)(intptr_t)lv_event_get_user_data(e), RAD_STEP, &tm)) commit();
}
static void on_card_mode(lv_event_t * e) {
    struct tm tm = now_tm();
    if (rad_cycle_mode(&s_set, (int)(intptr_t)lv_event_get_user_data(e), &tm)) commit();
}
static void on_card_edit(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_set.n_rooms) open_editor(s_set.rooms[idx].id);
}

/* Room card: accent stripe like the home tiles, name + status left, the
 * target big on the right, controls along the bottom. */
static void room_card(lv_obj_t * parent, int idx, lv_coord_t w, lv_coord_t h, const struct tm * tm) {
    const rad_room_t * r = &s_set.rooms[idx];
    rad_target_t t = rad_target_for(r, tm);

    lv_obj_t * c = plain(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, CARD_R, 0);

    lv_obj_t * stripe = plain(c);
    lv_obj_set_pos(stripe, SX(14), SY(10));
    lv_obj_set_size(stripe, w - SX(28), 4);
    lv_obj_set_style_radius(stripe, 2, 0);
    lv_obj_set_style_bg_color(stripe, lv_color_hex(reason_color(t.reason)), 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

    lv_obj_t * name = label(c, r->name, SF(22), THEME_TEXT_HI);
    one_line(name, w - SX(200));
    lv_obj_set_pos(name, SX(18), SY(26));

    char status[96];
    status_text(r, tm, status, sizeof status);
    lv_obj_t * st = label(c, status, SF(14), THEME_TEXT_DIM);
    one_line(st, w - SX(190));
    lv_obj_set_pos(st, SX(18), SY(60));

    char big[24];
    if (t.reason == RAD_REASON_OFF) snprintf(big, sizeof big, "%s", tr("Uit", "Off"));
    else { fmt_temp(t.target, big, sizeof big); strcat(big, "\xc2\xb0"); }
    lv_obj_t * temp = label(c, big, SF(48), t.reason == RAD_REASON_OFF ? THEME_TEXT_MUTED : COL_TEMP);
    lv_obj_align(temp, LV_ALIGN_TOP_RIGHT, -SX(18), SY(22));

    lv_coord_t by = h - BTN - SY(14);
    button(c, SX(14), by, BTN, BTN, THEME_BTN_BG, THEME_TEXT_HI, LV_SYMBOL_MINUS, SF(22), on_card_minus, idx);
    button(c, SX(14) + BTN + SX(8), by, BTN, BTN, THEME_BTN_BG, THEME_TEXT_HI, LV_SYMBOL_PLUS, SF(22), on_card_plus, idx);
    uint32_t mc = mode_color(r->mode);
    button(c, SX(14) + 2 * (BTN + SX(8)) + SX(8), by, SX(150), BTN, mc,
           r->mode == RAD_MODE_OFF ? THEME_TEXT_HI : 0xffffff, mode_label(r->mode), SF(18), on_card_mode, idx);
    char edit[48];
    snprintf(edit, sizeof edit, LV_SYMBOL_SETTINGS "  %s", tr("Instellen", "Settings"));
    button(c, w - SX(14) - SX(146), by, SX(146), BTN, THEME_BTN_BG, THEME_TEXT_HI, edit, SF(18), on_card_edit, idx);
}

static void build_overview(lv_obj_t * body, const struct tm * tm) {
    back_button(body, on_close);

    lv_obj_t * title = label(body, tr("Radiatoren", "Radiators"), SF(28), THEME_TEXT_HI);
    lv_obj_set_pos(title, SX(168), SY(18));
    char sub[64];
    if (s_set.n_rooms == 1) snprintf(sub, sizeof sub, "%s", tr("1 kamer", "1 room"));
    else snprintf(sub, sizeof sub, tr("%d kamers", "%d rooms"), s_set.n_rooms);
    lv_obj_t * subl = label(body, sub, SF(14), THEME_TEXT_DIM);
    lv_obj_set_pos(subl, SX(170), SY(50));

    char add[48];
    snprintf(add, sizeof add, LV_SYMBOL_PLUS "  %s", tr("Kamer", "Room"));
    lv_obj_t * addb = button(body, LV_HOR_RES - PAD - SX(170), SY(12), SX(170), HEAD_H, COL_ACCENT, 0xffffff,
                             add, SF(20), on_add_room, 0);
    if (s_set.n_rooms >= RAD_MAX_ROOMS) lv_obj_add_state(addb, LV_STATE_DISABLED);

    /* Info strip: how valves are linked. */
    lv_obj_t * info = card(body, PAD, SY(80), LV_HOR_RES - 2 * PAD, SY(42), THEME_ROW_BG);
    lv_obj_set_style_radius(info, 10, 0);
    lv_obj_t * ii = label(info, LV_SYMBOL_HOME, SF(18), s_save_failed ? COL_DANGER : COL_ACCENT);
    lv_obj_align(ii, LV_ALIGN_LEFT_MID, SX(14), 0);
    lv_obj_t * it = label(info, s_save_failed
        ? tr("Opslaan mislukt: kan /mnt/data/tsc niet schrijven.", "Saving failed: cannot write /mnt/data/tsc.")
        : tr("Radiatorknoppen koppel je in Home Assistant: zet een knop in de ruimte met dezelfde naam als de kamer.",
             "Radiator valves are linked in Home Assistant: put a valve in the area with the same name as the room."),
        SF(14), s_save_failed ? COL_DANGER : THEME_TEXT_DIM);
    one_line(it, LV_HOR_RES - 2 * PAD - SX(60));
    lv_obj_align(it, LV_ALIGN_LEFT_MID, SX(46), 0);

    lv_coord_t top = SY(134);
    if (s_set.n_rooms == 0) {
        lv_obj_t * c = card(body, 0, 0, SX(560), SY(300), THEME_CARD_BG);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, top + SY(40));
        lv_obj_t * badge = plain(c);
        lv_obj_set_size(badge, SX(72), SX(72));
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(COL_COMFORT), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
        lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, SY(30));
        lv_obj_t * img = lv_img_create(badge);
        lv_img_set_src(img, &icon_radiator);
        lv_obj_set_style_img_recolor(img, lv_color_hex(COL_COMFORT), 0);
        lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
        lv_obj_t * t = label(c, tr("Nog geen kamers", "No rooms yet"), SF(24), THEME_TEXT_HI);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, SY(118));
        lv_obj_t * d = label(c, tr("Geef elke kamer een eigen temperatuur en weekschema.",
                                   "Give every room its own temperature and weekly schedule."), SF(14), THEME_TEXT_DIM);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, SY(156));
        lv_obj_t * b = button(c, 0, 0, SX(280), BTN, COL_ACCENT, 0xffffff, add, SF(20), on_add_room, 0);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, SY(210));
        return;
    }

    s_scroll = scroller(body, PAD, top, LV_HOR_RES - 2 * PAD + SX(10), LV_VER_RES - top);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_scroll, SX(12), 0);
    lv_obj_set_style_pad_row(s_scroll, SY(12), 0);
    lv_obj_set_style_pad_bottom(s_scroll, SY(16), 0);
    lv_obj_set_style_pad_right(s_scroll, SX(10), 0);       /* room for the scrollbar */
    lv_coord_t cw = (LV_HOR_RES - 2 * PAD - SX(12)) / 2;
    for (int i = 0; i < s_set.n_rooms; i++) room_card(s_scroll, i, cw, SY(180), tm);
}

/* ======================================================================== */
/* editor                                                                   */
/* ======================================================================== */

static void on_editor_back(lv_event_t * e) {
    (void)e;
    s_view = VIEW_OVERVIEW;
    s_keep_scroll = -1;
    request_rebuild();
}

static void on_temp(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);     /* field<<1 | up */
    int idx = edit_index();
    double delta = (d & 1) ? RAD_STEP : -RAD_STEP;
    int changed = (d >> 1) == 0 ? rad_nudge_comfort(&s_set, idx, delta) : rad_nudge_eco(&s_set, idx, delta);
    if (changed) commit();
}

static void on_now(lv_event_t * e) {
    struct tm tm = now_tm();
    double delta = (intptr_t)lv_event_get_user_data(e) ? RAD_STEP : -RAD_STEP;
    if (rad_nudge_target(&s_set, edit_index(), delta, &tm)) commit();
}

static void on_mode(lv_event_t * e) {
    struct tm tm = now_tm();
    if (rad_set_mode(&s_set, edit_index(), (rad_mode_t)(intptr_t)lv_event_get_user_data(e), &tm)) commit();
}

static void on_add_block(lv_event_t * e) {
    (void)e;
    if (rad_add_block(&s_set, edit_index())) { s_keep_scroll = 10000; commit(); }
}

static void on_del_block(lv_event_t * e) {
    if (rad_remove_block(&s_set, edit_index(), (int)(intptr_t)lv_event_get_user_data(e))) commit();
}

static void on_day(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);     /* block<<4 | day */
    if (rad_toggle_block_day(&s_set, edit_index(), (int)(d >> 4), (int)(d & 15))) commit();
}

static void on_time(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);     /* block<<2 | is_end<<1 | up */
    int delta = (d & 1) ? 15 : -15;
    if (rad_nudge_block_time(&s_set, edit_index(), (int)(d >> 2), (int)((d >> 1) & 1), delta)) commit();
}

/* One "-  20,5°  +" stepper. */
static void stepper(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, double value, uint32_t color,
                    lv_event_cb_t cb, intptr_t down, intptr_t up) {
    button(parent, x, y, BTN, BTN, THEME_BTN_BG, THEME_TEXT_HI, LV_SYMBOL_MINUS, SF(22), cb, down);
    char v[24];
    fmt_temp(value, v, sizeof v);
    strcat(v, "\xc2\xb0");
    lv_obj_t * l = label(parent, v, SF(28), color);
    lv_obj_set_width(l, SX(96));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, x + BTN, y + (BTN - lv_font_get_line_height(SF(28))) / 2);
    button(parent, x + BTN + SX(96), y, BTN, BTN, THEME_BTN_BG, THEME_TEXT_HI, LV_SYMBOL_PLUS, SF(22), cb, up);
}

static void temp_row(lv_obj_t * parent, lv_coord_t y, uint32_t dot, const char * name, const char * hint,
                     double value, lv_event_cb_t cb, intptr_t down, intptr_t up) {
    lv_obj_t * d = plain(parent);
    lv_obj_set_size(d, SX(10), SX(10));
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(dot), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_pos(d, SX(20), y + SY(14));
    lv_obj_t * n = label(parent, name, SF(20), THEME_TEXT_HI);
    lv_obj_set_pos(n, SX(38), y + SY(4));
    lv_obj_t * h = label(parent, hint, SF(12), THEME_TEXT_DIM);
    lv_obj_set_pos(h, SX(38), y + SY(32));
    stepper(parent, SX(420) - SX(16) - 2 * BTN - SX(96), y, value, COL_TEMP, cb, down, up);
}

/* Seven 24-hour bars, comfort blocks drawn over eco, with a "now" marker. */
static void week_overview(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          const rad_room_t * r, const struct tm * tm) {
    const lv_coord_t lab_w = SX(42), row_h = SY(12), gap = SY(5);
    lv_coord_t bar_x = x + lab_w, bar_w = w - lab_w;
    int today = rad_day_index(tm);
    for (int d = 0; d < 7; d++) {
        lv_coord_t ry = y + d * (row_h + gap);
        lv_obj_t * l = label(parent, day_short(d), SF(12), d == today ? THEME_TEXT_HI : THEME_TEXT_DIM);
        lv_obj_set_pos(l, x, ry - SY(2));
        lv_obj_t * bar = plain(parent);
        lv_obj_set_pos(bar, bar_x, ry);
        lv_obj_set_size(bar, bar_w, row_h);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ECO), 0);
        lv_obj_set_style_bg_opa(bar, r->mode == RAD_MODE_AUTO ? LV_OPA_40 : LV_OPA_20, 0);
        lv_obj_set_style_clip_corner(bar, true, 0);
        for (int b = 0; b < r->n_blocks; b++) {
            const rad_block_t * bl = &r->blocks[b];
            if (!bl->days[d]) continue;
            lv_obj_t * seg = plain(bar);
            lv_coord_t sx = (lv_coord_t)((long)bar_w * bl->start / 1440);
            lv_coord_t ex = (lv_coord_t)((long)bar_w * bl->end / 1440);
            lv_obj_set_pos(seg, sx, 0);
            lv_obj_set_size(seg, ex - sx > 2 ? ex - sx : 2, row_h);
            lv_obj_set_style_bg_color(seg, lv_color_hex(COL_COMFORT), 0);
            lv_obj_set_style_bg_opa(seg, r->mode == RAD_MODE_AUTO ? LV_OPA_COVER : LV_OPA_40, 0);
        }
        if (d == today) {
            lv_obj_t * now = plain(parent);
            lv_coord_t nx = bar_x + (lv_coord_t)((long)bar_w * rad_minutes_of_day(tm) / 1440);
            lv_obj_set_pos(now, nx - 1, ry - SY(3));
            lv_obj_set_size(now, 3, row_h + SY(6));
            lv_obj_set_style_radius(now, 1, 0);
            lv_obj_set_style_bg_color(now, lv_color_hex(THEME_TEXT_HI), 0);
            lv_obj_set_style_bg_opa(now, LV_OPA_COVER, 0);
        }
    }
    lv_coord_t ty = y + 7 * (row_h + gap);
    static const char * const ticks[] = { "0", "6", "12", "18", "24" };
    for (int i = 0; i < 5; i++) {
        lv_obj_t * t = label(parent, ticks[i], SF(12), THEME_TEXT_MUTED);
        lv_obj_update_layout(t);
        lv_coord_t tx = bar_x + bar_w * i / 4 - lv_obj_get_width(t) / 2;
        if (i == 4) tx = bar_x + bar_w - lv_obj_get_width(t);
        if (i == 0) tx = bar_x;
        lv_obj_set_pos(t, tx, ty);
    }
}

static void block_row(lv_obj_t * parent, int b, const rad_block_t * bl, lv_coord_t w) {
    lv_obj_t * row = plain(parent);
    lv_obj_set_size(row, w, SY(12) + BTN + SY(10) + BTN + SY(12));
    lv_obj_set_style_bg_color(row, lv_color_hex(THEME_ROW_BG), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);

    lv_coord_t chip = BTN, cgap = SX(6);
    for (int d = 0; d < 7; d++) {
        int on = bl->days[d];
        button(row, SX(12) + d * (chip + cgap), SY(12), chip, BTN,
               on ? COL_ACCENT : THEME_BTN_BG, on ? 0xffffff : THEME_TEXT_DIM,
               day_short(d), SF(18), on_day, (intptr_t)(b << 4 | d));
    }
    button(row, w - SX(12) - BTN, SY(12), BTN, BTN, THEME_BTN_BG, COL_DANGER, LV_SYMBOL_TRASH, SF(20),
           on_del_block, b);

    lv_coord_t y2 = SY(12) + BTN + SY(10);
    lv_coord_t ty = y2 + (BTN - lv_font_get_line_height(SF(24))) / 2;
    lv_obj_t * from = label(row, tr("van", "from"), SF(14), THEME_TEXT_DIM);
    lv_obj_set_pos(from, SX(14), y2 + (BTN - lv_font_get_line_height(SF(14))) / 2);
    lv_obj_t * to = label(row, tr("tot", "to"), SF(14), THEME_TEXT_DIM);

    for (int which = 0; which < 2; which++) {
        lv_coord_t x0 = which == 0 ? SX(56) : SX(300);
        char hm[8];
        rad_format_minutes(which == 0 ? bl->start : bl->end, hm, sizeof hm);
        button(row, x0, y2, BTN, BTN, THEME_BTN_BG, THEME_TEXT_HI, LV_SYMBOL_MINUS, SF(20), on_time,
               (intptr_t)(b << 2 | which << 1 | 0));
        lv_obj_t * t = label(row, hm, SF(24), THEME_TEXT_HI);
        lv_obj_set_width(t, SX(88));
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(t, x0 + BTN, ty);
        button(row, x0 + BTN + SX(88), y2, BTN, BTN, THEME_BTN_BG, THEME_TEXT_HI, LV_SYMBOL_PLUS, SF(20), on_time,
               (intptr_t)(b << 2 | which << 1 | 1));
    }
    lv_obj_set_pos(to, SX(262), y2 + (BTN - lv_font_get_line_height(SF(14))) / 2);
}

static void build_editor(lv_obj_t * body, const struct tm * tm) {
    int idx = edit_index();
    if (idx < 0) { s_view = VIEW_OVERVIEW; build_overview(body, tm); return; }
    const rad_room_t * r = &s_set.rooms[idx];
    rad_target_t t = rad_target_for(r, tm);

    /* ---- header: back, name (tap to rename), delete --------------------- */
    back_button(body, on_editor_back);
    lv_obj_t * name = plain(body);
    lv_obj_set_pos(name, SX(164), SY(12));
    lv_obj_set_size(name, SX(600), HEAD_H);
    lv_obj_add_flag(name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(name, open_rename, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = label(name, r->name, SF(28), THEME_TEXT_HI);
    lv_point_t tsz;
    lv_txt_get_size(&tsz, r->name, SF(28), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    one_line(nl, tsz.x < SX(500) ? tsz.x + 2 : SX(500));
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, SX(4), 0);
    lv_obj_t * pen = button(name, 0, 0, BTN, BTN - SY(8), THEME_BTN_BG, THEME_TEXT_DIM, LV_SYMBOL_EDIT, SF(18),
                            open_rename, 0);
    lv_obj_update_layout(nl);
    lv_obj_align_to(pen, nl, LV_ALIGN_OUT_RIGHT_MID, SX(12), 0);

    char del[48];
    snprintf(del, sizeof del, LV_SYMBOL_TRASH "  %s", tr("Verwijderen", "Delete"));
    button(body, LV_HOR_RES - PAD - SX(182), SY(12), SX(182), HEAD_H, COL_DANGER, 0xffffff, del, SF(18),
           open_delete, 0);

    /* ---- left card: temperatures, mode, now, week ------------------------ */
    const lv_coord_t top = SY(80), ch = LV_VER_RES - top - SY(12), lw = SX(420);
    lv_obj_t * left = card(body, PAD, top, lw, ch, THEME_CARD_BG);

    temp_row(left, SY(14), COL_COMFORT, "Comfort", tr("binnen de tijdblokken", "inside the time blocks"),
             r->comfort, on_temp, 0 << 1 | 0, 0 << 1 | 1);
    temp_row(left, SY(14) + BTN + SY(10), COL_ECO, "Eco", tr("buiten de tijdblokken", "outside the time blocks"),
             r->eco, on_temp, 1 << 1 | 0, 1 << 1 | 1);
    divider(left, SX(16), SY(150), lw - SX(32));

    /* Mode: segmented control. */
    lv_coord_t seg_y = SY(164), seg_w = (lw - SX(32) - 2 * SX(6)) / 3;
    for (int m = 0; m < 3; m++) {
        int sel = (int)r->mode == m;
        button(left, SX(16) + m * (seg_w + SX(6)), seg_y, seg_w, BTN,
               sel ? mode_color((rad_mode_t)m) : THEME_BTN_BG,
               sel ? ((rad_mode_t)m == RAD_MODE_OFF ? THEME_TEXT_HI : 0xffffff) : THEME_TEXT_DIM,
               mode_label((rad_mode_t)m), SF(18), on_mode, m);
    }

    /* Target now, adjustable like on the overview (switches to manual). */
    lv_coord_t now_y = seg_y + BTN + SY(14);
    lv_obj_t * nowl = label(left, tr("Nu", "Now"), SF(20), THEME_TEXT_HI);
    lv_obj_set_pos(nowl, SX(20), now_y + SY(4));
    char status[96];
    status_text(r, tm, status, sizeof status);
    lv_obj_t * nows = label(left, status, SF(12), THEME_TEXT_DIM);
    one_line(nows, SX(170));
    lv_obj_set_pos(nows, SX(20), now_y + SY(32));
    stepper(left, lw - SX(16) - 2 * BTN - SX(96), now_y, t.target,
            t.reason == RAD_REASON_OFF ? THEME_TEXT_MUTED : COL_TEMP, on_now, 0, 1);
    divider(left, SX(16), now_y + BTN + SY(12), lw - SX(32));

    lv_coord_t wk_y = now_y + BTN + SY(24);
    lv_obj_t * wl = label(left, tr("Week", "Week"), SF(14), THEME_TEXT_DIM);
    lv_obj_set_pos(wl, SX(20), wk_y);
    week_overview(left, SX(20), wk_y + SY(26), lw - SX(40), r, tm);

    /* ---- right card: time blocks ----------------------------------------- */
    lv_coord_t rx = PAD + lw + SX(12), rw = LV_HOR_RES - rx - PAD;
    lv_obj_t * right = card(body, rx, top, rw, ch, THEME_CARD_BG);
    lv_obj_t * bh = label(right, tr("Tijdblokken met comforttemperatuur", "Time blocks at comfort temperature"),
                          SF(18), THEME_TEXT_HI);
    lv_obj_set_pos(bh, SX(18), SY(18));
    char ha[96];
    snprintf(ha, sizeof ha, LV_SYMBOL_HOME "  %s \"%s\"", tr("Knoppen: ruimte", "Valves: area"), r->name);
    lv_obj_t * hal = label(right, ha, SF(14), THEME_TEXT_DIM);
    one_line(hal, rw - SX(200));
    lv_obj_set_pos(hal, SX(18), SY(46));
    if (r->n_blocks < RAD_MAX_BLOCKS) {
        char add[40];
        snprintf(add, sizeof add, LV_SYMBOL_PLUS "  %s", tr("Blok", "Block"));
        button(right, rw - SX(16) - SX(140), SY(14), SX(140), BTN, COL_ACCENT, 0xffffff, add, SF(18),
               on_add_block, 0);
    }

    lv_coord_t ly = SY(84);
    s_scroll = scroller(right, SX(12), ly, rw - SX(12), ch - ly);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scroll, SY(10), 0);
    lv_obj_set_style_pad_bottom(s_scroll, SY(12), 0);
    if (r->n_blocks == 0) {
        lv_obj_t * e = label(s_scroll, tr("Geen tijdblokken: de kamer blijft op de eco-temperatuur.",
                                          "No time blocks: the room stays at the eco temperature."),
                             SF(18), THEME_TEXT_DIM);
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e, rw - SX(48));
        lv_obj_set_style_pad_top(e, SY(40), 0);
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
    }
    for (int b = 0; b < r->n_blocks; b++) block_row(s_scroll, b, &r->blocks[b], rw - SX(36));
}

/* ======================================================================== */
/* screen                                                                   */
/* ======================================================================== */

static void rebuild(void) {
    s_rebuild_pending = 0;
    if (!s_root) return;
    lv_coord_t keep = s_keep_scroll;
    if (keep < 0 && s_scroll) keep = lv_obj_get_scroll_y(s_scroll);
    s_keep_scroll = -1;

    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    if (s_body) lv_obj_del(s_body);
    s_scroll = NULL;
    s_body = plain(s_root);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES);
    if (s_modal) lv_obj_move_foreground(s_modal);

    struct tm tm = now_tm();
    if (s_view == VIEW_EDITOR) build_editor(s_body, &tm);
    else build_overview(s_body, &tm);

    if (s_scroll && keep > 0) {
        lv_obj_update_layout(s_scroll);
        lv_obj_scroll_to_y(s_scroll, keep, LV_ANIM_OFF);
    }
}

static void rebuild_async(void * p) { (void)p; if (s_rebuild_pending) rebuild(); }

/* Never delete the widget whose event is running: rebuild on the next tick. */
static void request_rebuild(void) {
    if (s_rebuild_pending || !s_root) return;
    s_rebuild_pending = 1;
    lv_async_call(rebuild_async, NULL);
}

static void on_tick(lv_timer_t * t) {
    (void)t;
    if (s_modal || lv_scr_act() != s_root) return;   /* don't disturb typing, nor redraw while hidden */
    request_rebuild();
}

static void on_root_deleted(lv_event_t * e) {
    (void)e;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_root = s_body = s_scroll = s_modal = s_name_ta = NULL;
    s_rebuild_pending = 0;
}

EXPORT lv_obj_t * screen_create(void) {
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }

    host_settings_refresh();
    rad_load_settings(&s_set, settings_path());
    s_view = VIEW_OVERVIEW;
    s_edit_id[0] = 0;
    s_save_failed = 0;
    s_keep_scroll = -1;

    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);

    rebuild();
    s_timer = lv_timer_create(on_tick, 20000, NULL);   /* schedule edges, "now" marker */
    return s_root;
}
