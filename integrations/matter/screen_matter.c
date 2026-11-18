/*
 * screen_matter.c - FreeToon UI plugin (matter_ui.<toon>.so) for the Matter
 * Bridge. toonui dlopen()s this when the Matter tile is tapped and calls
 * screen_create().
 *
 * Left:  pairing - the QR code and the setup code for Apple Home or Google
 *        Home while the bridge waits for a controller; once paired, what to
 *        do next; while the bridge is down or starting, why.
 * Right: the service and pairing state (a small dot with text), the devices
 *        to share (a switch per device, written straight to
 *        /mnt/data/tsc/matter-bridge.json, which the bridge reads every 5 s)
 *        and two quiet buttons with a confirmation: restart the service and
 *        remove the pairing (commissioning reset). Both run in the
 *        integration's daemon, never in toonui.
 *
 * Look: FreeToon's theme tokens (theme.h, docs/DESIGN.md) only - surfaces
 * separated by a calm difference in fill rather than outlines, one accent,
 * tracked small labels, light numerals, an 8 px grid. The QR code is drawn by LVGL (lv_qrcode, part of
 * toonui) from the payload the bridge itself publishes in status.json.
 */
#define _GNU_SOURCE
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "fonts_opensans.h"     /* THEME_FONT_NUM: Open Sans Light readouts */
#include "plugin_abi.h"
#include "matter_logic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Settings fields this plugin touches (tr() reads `language`; the THEME_*
 * tokens fall back to `home_light_mode` on a toonui without themes). */
FT_PLUGIN_DECLARE_ABI(
    FT_SETTINGS_FIELD(language),
    FT_SETTINGS_FIELD(home_light_mode));

#define EXPORT __attribute__((visibility("default")))

/* ---- geometry: 8 px grid in the 1024x600 design space (SX/SY for Toon 1) - */
#define G(n)        THEME_SP(n)
#define MARGIN      SX(G(3))                 /* 24: screen edge */
#define GUTTER      SX(G(3))                 /* 24: between the columns */
#define TOP         SY(G(11))                /* 88: below the header */
#define BOTTOM      SY(600 - G(3))           /* 576 */
#define LEFT_W      SX(G(58))                /* 464 */
#define RIGHT_X     (MARGIN + LEFT_W + GUTTER)
#define RIGHT_W     (SX(1024) - RIGHT_X - MARGIN)
#define INSET       SX(G(3))                 /* 24: inside a card */
#define BTN_H       SY(G(7))                 /* 56: touch target */
#define QR_SIZE     SX(200)                  /* 25 modules (version 2) x 8 px */
#define QR_QUIET    SX(G(3))                 /* light margin round the code: scanners need ~4 modules */

/* ---- state ---------------------------------------------------------------- */
static mb_snapshot_t s_snap;
static mb_paths_t    s_paths;
static unsigned long s_sig;
static int           s_save_failed;
static int           s_cmd_error;         /* daemon not running when asked */
static mb_action_t   s_sent;              /* sent, daemon has not picked it up yet */
static time_t        s_sent_at;

static lv_obj_t *   s_root;
static lv_obj_t *   s_body;
static lv_obj_t *   s_scroll;
static lv_obj_t *   s_modal;
static lv_obj_t *   s_energy_lbl;
static lv_timer_t * s_timer;
static int          s_rebuild_pending;

static void rebuild(void);
static void request_rebuild(void);

/* ======================================================================== */
/* data                                                                     */
/* ======================================================================== */

static void load(void) {
    mb_default_paths(&s_paths);
#ifdef MB_HOST_PREVIEW
    /* host preview: every file in one folder */
    static char buf[7][300];
    const char * root = getenv("MATTER_ROOT");
    if (root && root[0]) {
        const char * names[7] = { "status.json", "prefs.json", "comfort.json", "service.pid", "service.log",
                                  "action.json", "service.sh" };
        for (int i = 0; i < 7; i++) snprintf(buf[i], sizeof buf[i], "%s/%s", root, names[i]);
        s_paths.status = buf[0]; s_paths.prefs = buf[1]; s_paths.comfort = buf[2]; s_paths.pidfile = buf[3];
        s_paths.service_log = buf[4]; s_paths.action = buf[5]; s_paths.script = buf[6];
    }
#endif
    mb_snapshot_load(&s_snap, &s_paths, settings.language == LANG_EN);
    if (s_sent && (s_snap.action != MB_ACT_IDLE || time(NULL) - s_sent_at > 6)) s_sent = MB_ACT_IDLE;
}

static unsigned long hash_str(unsigned long h, const char * s) {
    while (s && *s) h = h * 33 + (unsigned char)*s++;
    return h * 33;
}

/* Everything the layout shows, except the live energy figure (updated in
 * place). A change of theme or language rebuilds too. */
static unsigned long signature(void) {
    const mb_snapshot_t * s = &s_snap;
    unsigned long h = 5381;
    h = h * 33 + (unsigned long)s->service;
    h = h * 33 + (unsigned long)s->commissioned;
    h = h * 33 + (unsigned long)s->action;
    h = h * 33 + (unsigned long)s_sent;
    h = h * 33 + (unsigned long)s->daemon_alive;
    h = h * 33 + (unsigned long)s->autostart_paused;
    h = h * 33 + (unsigned long)s->last_failed;
    h = h * 33 + (unsigned long)s->last_done;
    h = h * 33 + (unsigned long)s->n_devices;
    h = h * 33 + (unsigned long)settings.language;
    h = h * 33 + (unsigned long)THEME_BG + (unsigned long)THEME_ACCENT;
    h = h * 33 + (unsigned long)s_save_failed + 7UL * (unsigned long)s_cmd_error;
    h = hash_str(h, s->pairing_code);
    h = hash_str(h, s->qr_payload);
    h = hash_str(h, s->message);
    h = hash_str(h, s->log_error);
    for (int i = 0; i < s->n_devices; i++) {
        h = hash_str(h, s->devices[i].id);
        h = hash_str(h, s->devices[i].name);
        h = h * 33 + (unsigned long)(s->devices[i].selected * 2 + s->devices[i].online);
    }
    return h;
}

static mb_action_t busy_action(void) {
    return s_snap.action != MB_ACT_IDLE ? s_snap.action : s_sent;
}

/* ======================================================================== */
/* widget factories                                                         */
/* ======================================================================== */

static lv_obj_t * plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t * surface(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                          uint32_t bg, lv_coord_t radius) {
    lv_obj_t * c = plain(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, radius, 0);
    return c;
}

static lv_obj_t * card(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    return surface(parent, x, y, w, h, THEME_CARD_BG, THEME_RADIUS_TILE);
}

static lv_obj_t * label(lv_obj_t * parent, const char * text, const lv_font_t * font, uint32_t color) {
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Small tracked label: section and field names (sentence case). */
static lv_obj_t * caption(lv_obj_t * parent, const char * text) {
    lv_obj_t * l = label(parent, text, THEME_FONT_LABEL, THEME_TEXT_DIM);
    lv_obj_set_style_text_letter_space(l, THEME_LABEL_TRACK, 0);
    return l;
}

static lv_obj_t * wrapped(lv_obj_t * parent, const char * text, const lv_font_t * font, uint32_t color,
                          lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t * l = label(parent, text, font, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_line_space(l, 4, 0);
    return l;
}

static lv_obj_t * centered(lv_obj_t * parent, const char * text, const lv_font_t * font, uint32_t color,
                           lv_coord_t y, lv_coord_t w) {
    lv_obj_t * l = label(parent, text, font, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 4, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static void one_line(lv_obj_t * l, lv_coord_t w) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

static lv_obj_t * button(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                         uint32_t bg, uint32_t fg, const char * text, lv_event_cb_t cb, intptr_t data) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, THEME_RADIUS_BTN, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(b, SX(4));
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)data);
    if (text) {
        lv_obj_t * l = label(b, text, THEME_FONT_BODY, fg);
        lv_obj_center(l);
    }
    return b;
}

static lv_obj_t * dot(lv_obj_t * parent, uint32_t color) {
    lv_obj_t * d = plain(parent);
    lv_obj_set_size(d, SX(10), SX(10));
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

/* A dot followed by text, right-aligned at (right edge x_right, top y). */
static void status_pair(lv_obj_t * parent, const char * text, uint32_t dot_color, uint32_t text_color,
                        lv_coord_t x_right, lv_coord_t y) {
    lv_obj_t * l = label(parent, text, THEME_FONT_BODY, text_color);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -x_right, y);
    lv_obj_update_layout(l);
    lv_obj_t * d = dot(parent, dot_color);
    lv_coord_t lh = lv_font_get_line_height(THEME_FONT_BODY);
    lv_obj_set_pos(d, lv_obj_get_x(l) - SX(G(2)), y + (lh - SX(10)) / 2);
}

static lv_obj_t * spinner(lv_obj_t * parent, lv_coord_t size) {
    lv_obj_t * sp = lv_spinner_create(parent, 1200, 70);
    lv_obj_set_size(sp, size, size);
    lv_obj_set_style_arc_color(sp, lv_color_hex(THEME_ROW_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_color_hex(THEME_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, SX(4), LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, SX(4), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(sp, true, LV_PART_INDICATOR);
    return sp;
}

/* A round emblem: a symbol on a soft tint of its colour. */
static lv_obj_t * emblem(lv_obj_t * parent, const char * symbol, uint32_t fg, uint32_t bg, lv_coord_t y) {
    lv_obj_t * c = plain(parent);
    lv_obj_set_size(c, SX(G(12)), SX(G(12)));
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t * s = label(c, symbol, THEME_FONT_LARGE, fg);
    lv_obj_center(s);
    return c;
}

/* ======================================================================== */
/* texts                                                                    */
/* ======================================================================== */

typedef struct { const char * text; uint32_t color; } state_text_t;

static state_text_t service_text(void) {
    mb_action_t busy = busy_action();
    if (busy == MB_ACT_RESET) return (state_text_t){ tr("Koppeling wissen", "Removing pairing"), THEME_WARNING };
    if (busy == MB_ACT_RESTART) return (state_text_t){ tr("Herstarten", "Restarting"), THEME_WARNING };
    if (busy == MB_ACT_START) return (state_text_t){ tr("Wordt gestart", "Starting"), THEME_WARNING };
    switch (s_snap.service) {
    case MB_SVC_RUNNING:  return (state_text_t){ tr("Actief", "Running"), THEME_ACCENT };
    case MB_SVC_STARTING: return (state_text_t){ tr("Start op", "Starting"), THEME_WARNING };
    case MB_SVC_STOPPED:  return (state_text_t){ tr("Gestopt", "Stopped"), THEME_DANGER };
    default:              return (state_text_t){ tr("Niet gevonden", "Not found"), THEME_TEXT_MUTED };
    }
}

static state_text_t pairing_text(void) {
    if (s_snap.service != MB_SVC_RUNNING || busy_action() != MB_ACT_IDLE)
        return (state_text_t){ tr("Onbekend", "Unknown"), THEME_TEXT_MUTED };
    if (s_snap.commissioned) return (state_text_t){ tr("Gekoppeld", "Paired"), THEME_ACCENT };
    return (state_text_t){ tr("Wacht op koppelen", "Waiting to pair"), THEME_TEXT_MUTED };
}

static state_text_t header_text(void) {
    if (busy_action() != MB_ACT_IDLE || s_snap.service != MB_SVC_RUNNING) return service_text();
    if (s_snap.commissioned) return (state_text_t){ tr("Gekoppeld", "Paired"), THEME_ACCENT };
    return (state_text_t){ tr("Klaar om te koppelen", "Ready to pair"), THEME_ACCENT };
}

/* One or two lines under the state rows: what just happened, the bridge's
 * own message, or an error. */
static void note_text(char * out, size_t n, uint32_t * color) {
    *color = THEME_TEXT_DIM;
    out[0] = 0;
    time_t now = time(NULL);
    if (s_cmd_error) {
        snprintf(out, n, "%s", tr("De achtergronddienst van deze app draait niet. Probeer het zo opnieuw.",
                                  "This app's background service is not running. Try again shortly."));
        *color = THEME_DANGER;
        return;
    }
    if (s_save_failed) {
        snprintf(out, n, "%s", tr("Je keuze kon niet worden opgeslagen.", "Your selection could not be saved."));
        *color = THEME_DANGER;
        return;
    }
    if (busy_action() != MB_ACT_IDLE) return;
    if (s_snap.last_done && now - s_snap.last_done < 300
        && (s_snap.last_action == MB_ACT_RESTART || s_snap.last_action == MB_ACT_RESET)) {
        int reset = s_snap.last_action == MB_ACT_RESET;
        if (s_snap.last_failed) {
            snprintf(out, n, "%s%s%s", reset ? tr("Wissen mislukt", "Reset failed") : tr("Herstart mislukt", "Restart failed"),
                     s_snap.last_message[0] ? ": " : ".", s_snap.last_message);
            *color = THEME_DANGER;
        } else if (s_snap.service == MB_SVC_RUNNING) {     /* not while it is still starting */
            snprintf(out, n, "%s", reset ? tr("Koppeling gewist. Verwijder de bridge ook in je Woning-app.",
                                              "Pairing removed. Also remove the bridge in your Home app.")
                                         : tr("Service opnieuw gestart.", "Service restarted."));
        }
        return;
    }
    if (s_snap.service == MB_SVC_STOPPED || s_snap.service == MB_SVC_MISSING) {
        if (s_snap.log_error[0]) {
            snprintf(out, n, "%s", s_snap.log_error);
            *color = THEME_DANGER;
        } else if (s_snap.service == MB_SVC_STOPPED) {
            snprintf(out, n, "%s", s_snap.autostart_paused
                     ? tr("Automatisch starten lukte niet. Volgende poging over een half uur.",
                          "Automatic start did not work. Next try in half an hour.")
                     : tr("FreeToon start de service zo vanzelf opnieuw.", "FreeToon starts the service again shortly."));
        }
        return;
    }
    if (s_snap.service == MB_SVC_RUNNING && s_snap.message[0]) {
        snprintf(out, n, "%s", s_snap.message);
        if (strstr(s_snap.message, "mislukt") || strstr(s_snap.message, "failed")) *color = THEME_DANGER;
    }
}

/* ======================================================================== */
/* modal: confirmation                                                      */
/* ======================================================================== */

static void modal_close(void) {
    if (s_modal) { lv_obj_del_async(s_modal); s_modal = NULL; }
    request_rebuild();
}

static void on_modal_cancel(lv_event_t * e) { (void)e; modal_close(); }

static void on_confirm(lv_event_t * e) {
    mb_action_t a = (mb_action_t)(intptr_t)lv_event_get_user_data(e);
    s_cmd_error = 0;
    if (s_snap.daemon_alive && mb_send_command(MB_COMMAND_PATH, mb_action_name(a)) == 0) {
        s_sent = a;
        s_sent_at = time(NULL);
    } else {
        s_cmd_error = 1;
    }
    modal_close();
}

static void open_confirm(mb_action_t a) {
    if (s_modal) lv_obj_del(s_modal);
    s_modal = plain(s_root);
    lv_obj_set_size(s_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);      /* scrim, as in DESIGN.md */
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_60, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);      /* swallow taps behind it */
    lv_obj_add_event_cb(s_modal, on_modal_cancel, LV_EVENT_CLICKED, NULL);

    lv_coord_t w = SX(G(76)), h = SY(G(37)), in = SX(G(4));
    lv_obj_t * c = surface(s_modal, 0, 0, w, h, THEME_CARD_BG, THEME_RADIUS_PANEL);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(c);

    int reset = a == MB_ACT_RESET;
    lv_obj_t * t = label(c, reset ? tr("Koppeling wissen?", "Remove the pairing?")
                                  : tr("Service herstarten?", "Restart the service?"), THEME_FONT_HEADING, THEME_TEXT_HI);
    lv_obj_set_pos(t, in, SY(G(4)));
    wrapped(c, reset
            ? tr("Alle koppelingen met Apple Woning, Google Home en andere Matter-apps worden gewist. "
                 "Verwijder de bridge daarna ook in die apps; opnieuw koppelen kan met dezelfde code.",
                 "All pairings with Apple Home, Google Home and other Matter apps are removed. "
                 "Then remove the bridge in those apps too; you can pair again with the same code.")
            : tr("De bridge is ongeveer een minuut niet bereikbaar in je Woning-app. Koppelingen en je keuze "
                 "van apparaten blijven bewaard.",
                 "The bridge is unreachable in your Home app for about a minute. Pairings and your device "
                 "selection are kept."),
            THEME_FONT_BODY, THEME_TEXT_DIM, in, SY(G(10)), w - 2 * in);

    lv_coord_t bw = SX(G(22)), by = h - in - BTN_H;
    button(c, w - in - 2 * bw - SX(G(2)), by, bw, BTN_H, THEME_BTN_BG, THEME_TEXT_HI, tr("Annuleer", "Cancel"),
           on_modal_cancel, 0);
    button(c, w - in - bw, by, bw, BTN_H, reset ? THEME_DANGER_BG : THEME_SUCCESS_BG, THEME_ON_ACCENT,
           reset ? tr("Wissen", "Remove") : tr("Herstart", "Restart"), on_confirm, a);
}

static void on_restart(lv_event_t * e) { (void)e; open_confirm(MB_ACT_RESTART); }
static void on_reset(lv_event_t * e) { (void)e; open_confirm(MB_ACT_RESET); }

/* ======================================================================== */
/* screen                                                                   */
/* ======================================================================== */

static void on_close(lv_event_t * e) { (void)e; ui_pop(); }

static void on_device(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.n_devices) return;
    const mb_device_t * d = &s_snap.devices[idx];
    s_save_failed = mb_set_selected(s_paths.prefs, d->id, !d->selected) != 0;
    load();
    request_rebuild();
}

/* Left card: pairing. */
static void build_pairing(lv_obj_t * body) {
    lv_coord_t h = BOTTOM - TOP;
    lv_obj_t * c = card(body, MARGIN, TOP, LEFT_W, h);
    lv_coord_t tw = LEFT_W - 2 * INSET;
    mb_action_t busy = busy_action();

    if (busy != MB_ACT_IDLE || s_snap.service == MB_SVC_STARTING) {
        lv_obj_t * sp = spinner(c, SX(G(10)));
        lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, SY(G(16)));
        const char * head = busy == MB_ACT_RESET ? tr("Koppeling wordt gewist", "Removing the pairing")
                          : busy == MB_ACT_RESTART ? tr("Service wordt herstart", "Restarting the service")
                          : tr("Matter Bridge start op", "Matter Bridge is starting");
        centered(c, head, THEME_FONT_TITLE, THEME_TEXT_HI, SY(G(30)), tw);
        centered(c, busy == MB_ACT_RESET
                 ? tr("Dit duurt een paar minuten. Daarna verschijnt hier weer de code om te koppelen.",
                      "This takes a few minutes. The setup code then shows here again.")
                 : tr("Dit duurt ongeveer een minuut. De code om te koppelen verschijnt zodra de bridge klaar is.",
                      "This takes about a minute. The setup code shows once the bridge is ready."),
                 THEME_FONT_BODY, THEME_TEXT_DIM, SY(G(35)), tw);
        return;
    }

    if (s_snap.service == MB_SVC_MISSING || s_snap.service == MB_SVC_STOPPED) {
        int missing = s_snap.service == MB_SVC_MISSING;
        emblem(c, missing ? LV_SYMBOL_DOWNLOAD : LV_SYMBOL_POWER, THEME_TEXT_DIM, THEME_ROW_BG, SY(G(12)));
        centered(c, missing ? tr("Matter Bridge niet gevonden", "Matter Bridge not found")
                            : tr("Matter Bridge staat uit", "Matter Bridge is off"),
                 THEME_FONT_TITLE, THEME_TEXT_HI, SY(G(27)), tw);
#ifdef TOON1
        const char * why = tr("De Matter Bridge heeft een Toon 2 nodig: Node.js draait niet op een Toon 1.",
                              "The Matter Bridge needs a Toon 2: Node.js does not run on a Toon 1.");
#else
        const char * why = missing
            ? tr("Deze app gebruikt de service van de Matter Bridge-app van ToonSoftwareCollective. "
                 "Installeer die eerst; daarna verschijnt hier de code om te koppelen.",
                 "This app uses the service of the ToonSoftwareCollective Matter Bridge app. Install that "
                 "first; the setup code then shows here.")
            : tr("FreeToon start hem vanzelf opnieuw. Gaat dat niet, gebruik dan Herstart.",
                 "FreeToon starts it again by itself. If that does not work, use Restart.");
#endif
        centered(c, why, THEME_FONT_BODY, THEME_TEXT_DIM, SY(G(32)), tw);
        return;
    }

    if (s_snap.commissioned) {
        emblem(c, LV_SYMBOL_OK, THEME_ACCENT, THEME_ACCENT_SOFT, SY(G(7)));
        centered(c, tr("Gekoppeld", "Paired"), THEME_FONT_HEADING, THEME_TEXT_HI, SY(G(21)), tw);
        centered(c, tr("Apparaten die je hier deelt, verschijnen vanzelf in je Woning-app.",
                       "Devices you share here appear in your Home app by themselves."),
                 THEME_FONT_BODY, THEME_TEXT_DIM, SY(G(26)), tw);
        lv_obj_t * d = surface(c, INSET, SY(G(36)), tw, 1, THEME_DIVIDER, 0);
        (void)d;
        lv_obj_t * cap = caption(c, tr("Ook in een tweede app", "Also in a second app"));
        lv_obj_set_pos(cap, INSET, SY(G(38)));
        wrapped(c, tr("Zet in de eerste app koppelmodus aan voor de bridge en voeg hem met die code toe in de "
                      "tweede, bijvoorbeeld Google Home naast Apple Woning.",
                      "Turn on pairing mode for the bridge in the first app and add it to the second with that "
                      "code, say Google Home next to Apple Home."),
                THEME_FONT_CAPTION, THEME_TEXT_DIM, INSET, SY(G(41)), tw);
        return;
    }

    /* Waiting for a controller: the code on a light panel, the digits below.
     * The panel is light in every theme: phone scanners need dark on light. */
    lv_coord_t box = QR_SIZE + 2 * QR_QUIET;
    lv_obj_t * panel = surface(c, 0, 0, box, box, 0xffffff, THEME_RADIUS_TILE);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, SY(G(5)));
    if (s_snap.qr_payload[0]) {
        lv_obj_t * qr = lv_qrcode_create(panel, QR_SIZE, lv_color_black(), lv_color_white());
        lv_qrcode_update(qr, s_snap.qr_payload, (uint32_t)strlen(s_snap.qr_payload));
        lv_obj_center(qr);
    }

    lv_coord_t y = SY(G(5)) + box + SY(G(3));
    lv_obj_t * cap = caption(c, tr("Koppelcode", "Setup code"));
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y);

    /* 4-3-4, as Apple Home and Google Home print it */
    char code[24] = "";
    const char * pc = s_snap.pairing_code;
    if (strlen(pc) == 11) snprintf(code, sizeof code, "%.4s  %.3s  %.4s", pc, pc + 4, pc + 7);
    else snprintf(code, sizeof code, "%s", pc[0] ? pc : "-");
    lv_obj_t * cv = label(c, code, THEME_FONT_NUM(40), THEME_TEXT_HI);
    lv_obj_set_style_text_letter_space(cv, 2, 0);
    lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, y + SY(G(3)));

    centered(c, tr("Woning-app  " LV_SYMBOL_RIGHT "  +  " LV_SYMBOL_RIGHT "  Voeg accessoire toe. Scan de code of typ hem in. "
                   "Zelfde wifi als de Toon.",
                   "Home app  " LV_SYMBOL_RIGHT "  +  " LV_SYMBOL_RIGHT "  Add Accessory. Scan the code or type it. "
                   "Same Wi-Fi as the Toon."),
             THEME_FONT_CAPTION, THEME_TEXT_DIM, y + SY(G(10)), tw);
}

/* Right column: state, devices, actions. */
static void build_side(lv_obj_t * body) {
    /* ---- state: two rows and a note ---- */
    lv_coord_t sh = SY(G(19));                               /* 152 */
    lv_obj_t * st = card(body, RIGHT_X, TOP, RIGHT_W, sh);
    lv_coord_t row = SY(G(5));                               /* 40 */
    lv_coord_t lh = lv_font_get_line_height(THEME_FONT_BODY);
    struct { const char * name; state_text_t v; } rows[2] = {
        { tr("Service", "Service"), service_text() },
        { tr("Koppeling", "Pairing"), pairing_text() },
    };
    for (int i = 0; i < 2; i++) {
        lv_coord_t y = SY(G(2)) + i * row;
        lv_obj_t * l = caption(st, rows[i].name);
        lv_obj_set_pos(l, INSET, y + (lh - lv_font_get_line_height(THEME_FONT_CAPTION)) / 2);
        status_pair(st, rows[i].v.text, rows[i].v.color, THEME_TEXT_HI, INSET, y);
        if (i == 0) surface(st, INSET, y + row - SY(G(1)) + 1, RIGHT_W - 2 * INSET, 1, THEME_DIVIDER, 0);
    }
    char note[256];
    uint32_t nc;
    note_text(note, sizeof note, &nc);
    if (note[0]) {
        lv_obj_t * l = wrapped(st, note, THEME_FONT_CAPTION, nc, INSET, SY(G(2)) + 2 * row + SY(G(1)),
                               RIGHT_W - 2 * INSET);
        lv_obj_set_height(l, sh - (SY(G(2)) + 2 * row + SY(G(1))) - SY(G(1)));   /* two lines at most */
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }

    /* ---- devices ---- */
    lv_coord_t dy = TOP + sh + SY(G(2));
    lv_coord_t by = BOTTOM - BTN_H;
    lv_coord_t dh = by - SY(G(2)) - dy;
    lv_obj_t * dc = card(body, RIGHT_X, dy, RIGHT_W, dh);
    lv_obj_t * l = caption(dc, tr("Gedeeld via Matter", "Shared via Matter"));
    lv_obj_set_pos(l, INSET, SY(G(2)));
    char cnt[48];
    snprintf(cnt, sizeof cnt, tr("%d van %d", "%d of %d"), s_snap.n_selected, s_snap.n_devices);
    l = label(dc, cnt, THEME_FONT_CAPTION, THEME_TEXT_MUTED);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -INSET, SY(G(2)));

    lv_coord_t ly = SY(G(5));
    s_scroll = lv_obj_create(dc);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_pos(s_scroll, SX(G(1)), ly);
    lv_obj_set_size(s_scroll, RIGHT_W - SX(G(2)), dh - ly - SX(G(1)));
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(THEME_TEXT_MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_scroll, 4, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);

    lv_coord_t rw = RIGHT_W - SX(G(2)), rh = SY(G(8));
    s_energy_lbl = NULL;
    for (int i = 0; i < s_snap.n_devices; i++) {
        const mb_device_t * d = &s_snap.devices[i];
        lv_obj_t * r = plain(s_scroll);
        lv_obj_set_size(r, rw, rh);
        lv_obj_set_style_radius(r, THEME_RADIUS_BTN, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(THEME_ROW_BG), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, on_device, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i > 0) surface(r, SX(G(2)), 0, rw - SX(G(4)), 1, THEME_DIVIDER, 0);

        lv_obj_t * n = label(r, d->name, THEME_FONT_BODY, THEME_TEXT_HI);
        one_line(n, rw - SX(G(15)));
        lv_obj_set_pos(n, SX(G(2)), SY(G(1)) + SY(4));
        char detail[128];
        snprintf(detail, sizeof detail, "%s%s", d->detail, d->online ? "" : tr(", offline", ", offline"));
        lv_obj_t * dl = label(r, detail, THEME_FONT_CAPTION, d->online ? THEME_TEXT_DIM : THEME_OFFLINE);
        one_line(dl, rw - SX(G(15)));
        lv_obj_set_pos(dl, SX(G(2)), SY(G(4)) + SY(2));
        if (d->kind == MB_KIND_ENERGY) s_energy_lbl = dl;

        lv_obj_t * sw = lv_switch_create(r);
        lv_obj_set_size(sw, SX(G(7)), SY(G(4)));
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -SX(G(2)), 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);           /* the whole row toggles */
        lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_OFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_ACCENT_FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_ON_ACCENT), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sw, -SX(3), LV_PART_KNOB);
        if (d->selected) lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    /* ---- actions: two quiet buttons ---- */
    lv_coord_t bw = (RIGHT_W - SX(G(2))) / 2;
    int can = s_snap.service != MB_SVC_MISSING && busy_action() == MB_ACT_IDLE;
    char txt[64];
    snprintf(txt, sizeof txt, LV_SYMBOL_REFRESH "   %s", tr("Herstart", "Restart"));
    lv_obj_t * b1 = button(body, RIGHT_X, by, bw, BTN_H, THEME_BTN_BG, THEME_TEXT_HI, txt, on_restart, 0);
    snprintf(txt, sizeof txt, "%s", tr("Koppeling wissen", "Remove pairing"));
    lv_obj_t * b2 = button(body, RIGHT_X + bw + SX(G(2)), by, bw, BTN_H, THEME_BTN_BG, THEME_DANGER, txt,
                           on_reset, 0);
    if (!can) { lv_obj_add_state(b1, LV_STATE_DISABLED); lv_obj_add_state(b2, LV_STATE_DISABLED); }
}

static void build_header(lv_obj_t * body) {
    char txt[48];
    snprintf(txt, sizeof txt, LV_SYMBOL_LEFT "   %s", tr("Terug", "Back"));
    button(body, MARGIN, SY(G(2)), SX(G(16)), SY(G(6)), THEME_BTN_BG, THEME_TEXT_HI, txt, on_close, 0);
    lv_coord_t hh = lv_font_get_line_height(THEME_FONT_HEADING);
    lv_obj_t * title = label(body, "Matter", THEME_FONT_HEADING, THEME_TEXT_HI);
    lv_obj_set_pos(title, MARGIN + SX(G(19)), SY(G(2)) + (SY(G(6)) - hh) / 2);
    state_text_t hs = header_text();
    status_pair(body, hs.text, hs.color, THEME_TEXT_DIM, MARGIN,
                SY(G(2)) + (SY(G(6)) - lv_font_get_line_height(THEME_FONT_BODY)) / 2);
}

static void rebuild(void) {
    s_rebuild_pending = 0;
    if (!s_root) return;
    lv_coord_t keep = s_scroll ? lv_obj_get_scroll_y(s_scroll) : 0;

    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    if (s_body) lv_obj_del(s_body);
    s_scroll = NULL;
    s_energy_lbl = NULL;
    s_body = plain(s_root);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES);

    build_header(s_body);
    build_pairing(s_body);
    build_side(s_body);
    if (s_modal) lv_obj_move_foreground(s_modal);
    s_sig = signature();

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
    if (!s_root || lv_scr_act() != s_root) return;          /* hidden: nothing to draw */
    load();
    if (signature() != s_sig) {
        if (!s_modal) request_rebuild();
        return;
    }
    if (s_energy_lbl) {
        for (int i = 0; i < s_snap.n_devices; i++) {
            const mb_device_t * d = &s_snap.devices[i];
            if (d->kind != MB_KIND_ENERGY) continue;
            char detail[128];
            snprintf(detail, sizeof detail, "%s%s", d->detail, d->online ? "" : tr(", offline", ", offline"));
            if (strcmp(lv_label_get_text(s_energy_lbl), detail) != 0) lv_label_set_text(s_energy_lbl, detail);
        }
    }
}

static void on_root_deleted(lv_event_t * e) {
    (void)e;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_root = s_body = s_scroll = s_modal = s_energy_lbl = NULL;
    s_rebuild_pending = 0;
}

EXPORT lv_obj_t * screen_create(void) {
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }
    s_save_failed = 0;
    s_cmd_error = 0;
    s_sent = MB_ACT_IDLE;
    load();

    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);

    rebuild();
    s_timer = lv_timer_create(on_tick, 2000, NULL);
    return s_root;
}
