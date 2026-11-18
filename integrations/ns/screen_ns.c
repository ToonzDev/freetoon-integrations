/*
 * screen_ns.c - FreeToon UI plugin (ns_ui.<toon>.so): NS departure board.
 *
 * Reads /var/volatile/tmp/ns/state.json (written by the daemon every 15 s)
 * and shows the next train large (time, destination, track) with a calm
 * board of the trains after it. Delays and track changes are the only red.
 * The gear edits /mnt/data/integrations/ns/ns.conf (station, direction,
 * walk time, API key); the daemon reloads it and fetches straight away.
 */
#define _GNU_SOURCE
#define FTUI_ID "ns"
#include "ftui.h"
#include "ftjson.h"
#include "plugin_abi.h"

FT_PLUGIN_DECLARE_ABI(
    FT_SETTINGS_FIELD(language),
    FT_SETTINGS_FIELD(home_light_mode));

#define EXPORT __attribute__((visibility("default")))

#define STATE_PATH FTUI_RUN "/state.json"
#define CONF_PATH  FTUI_DIR "/ns.conf"
#define BOARD_ROWS 5

enum { VIEW_BOARD = 0, VIEW_SETTINGS = 1 };

static lv_obj_t *   s_root;
static lv_obj_t *   s_body;
static lv_timer_t * s_timer;
static int          s_view;
static char *       s_state_text;
static int          s_walk;             /* walk_min from state.json */

static const ftui_field_t FIELDS[] = {
    { "station", "Station", "Station",
      "Naam of code, bijvoorbeeld Utrecht Centraal of UT.", "Name or code, e.g. Utrecht Centraal or UT.", 0, 60 },
    { "direction", "Richting", "Direction",
      "Alleen treinen naar of via deze plaatsen, met komma's. Leeg = alle treinen.",
      "Only trains to or via these places, comma-separated. Empty = all trains.", 0, 120 },
    { "walk_min", "Looptijd naar het station", "Walk to the station",
      "In minuten. Treinen die je niet meer haalt verdwijnen.", "In minutes. Trains you can't catch are hidden.", 0, 3 },
    { "api_key", "NS API-sleutel", "NS API key",
      "apiportal.ns.nl, product Ns-App (primary key). Zonder sleutel: voorbeeldgegevens.",
      "apiportal.ns.nl, product Ns-App (primary key). Without a key: sample data.", 1, 64 },
    { "my_train", "Je vaste trein", "Your usual train",
      "Vertrektijd volgens de dienstregeling, bv. 07:42. Bij uitval of vertraging komt er een bericht. Leeg = uit.",
      "Scheduled departure, e.g. 07:42. A message when it is cancelled or late. Empty = off.", 0, 5 },
    { "warn_delay", "Waarschuwen vanaf", "Warn from",
      "Minuten vertraging voor een bericht over je vaste trein (standaard 5).",
      "Minutes of delay before a message about your train (default 5).", 0, 3 },
    { "morning", "Ochtendvenster", "Morning window",
      "Bv. 06:30-09:00: de volgende trein als bericht in de inbox. Leeg = uit.",
      "E.g. 06:30-09:00: the next train as an Inbox message. Empty = off.", 0, 11 },
};
#define N_CONN 4                                   /* first four: station / direction / walk / key */
#define N_FIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static void rebuild(void);

static void on_close(lv_event_t * e) { (void)e; ui_pop(); }
static void on_settings(lv_event_t * e) { (void)e; s_view = VIEW_SETTINGS; rebuild(); }

/* ---- board ------------------------------------------------------------------ */

static void hero(lv_obj_t * body, const jv * t, lv_coord_t y, lv_coord_t w) {
    lv_obj_t * c = ftui_card(body, FTUI_MARGIN, y, w, FTUI_GY(19), THEME_CARD_BG);
    int cancelled = jbool(t, "cancelled", 0);
    int delay = (int)jnum(t, "delay", 0);
    long in_min = (long)jnum(t, "in_min", 0);

    char when[48];
    long leave = in_min - s_walk;
    if (cancelled) snprintf(when, sizeof when, "%s", tr("Volgende trein, rijdt niet", "Next train, cancelled"));
    else if (s_walk > 0 && leave <= 0) snprintf(when, sizeof when, "%s", tr("Nu vertrekken", "Leave now"));
    else if (s_walk > 0) snprintf(when, sizeof when, tr("Vertrek over %ld min, %d min lopen", "Leave in %ld min, %d min walk"), leave, s_walk);
    else if (in_min <= 0) snprintf(when, sizeof when, "%s", tr("Vertrekt nu", "Leaving now"));
    else snprintf(when, sizeof when, tr("Volgende trein, over %ld min", "Next train, in %ld min"), in_min);
    ftui_caption(c, FTUI_G(3), FTUI_GY(2), when);

    lv_obj_t * tl = ftui_number(c, FTUI_G(3) - SX(4), FTUI_GY(5), jstr(t, "time", "--:--"), 90,
                                cancelled ? THEME_TEXT_MUTED : THEME_TEXT_HI);
    lv_obj_update_layout(tl);
    lv_coord_t after = FTUI_G(3) + lv_obj_get_width(tl);
    if (delay > 0 && !cancelled) {
        char d[16];
        snprintf(d, sizeof d, "+%d", delay);
        ftui_number(c, after + FTUI_G(1), FTUI_GY(7), d, 40, THEME_DANGER);
    }

    lv_coord_t dx = SX(392), dw = w - dx - SX(176);
    lv_obj_t * dl = ftui_label_at(c, dx, FTUI_GY(6), jstr(t, "dest", "?"), SF(28), cancelled ? THEME_TEXT_MUTED : THEME_TEXT_HI);
    ftui_one_line(dl, dw);
    char sub[160];
    const char * msg = jstr(t, "msg", "");
    if (cancelled || msg[0]) snprintf(sub, sizeof sub, "%s", msg[0] ? msg : tr("Rijdt niet", "Cancelled"));
    else {
        const char * via = jstr(t, "via", "");
        snprintf(sub, sizeof sub, "%s%s%s", jstr(t, "cat_long", ""), via[0] ? tr("  \xe2\x80\xa2  via ", "  \xe2\x80\xa2  via ") : "", via);
    }
    lv_obj_t * sl = ftui_label_at(c, dx, FTUI_GY(10), sub, SF(14), cancelled || msg[0] ? THEME_DANGER : THEME_TEXT_DIM);
    ftui_one_line(sl, dw);

    /* track, right */
    lv_coord_t rx = w - SX(152);
    int changed = jbool(t, "track_changed", 0);
    ftui_caption(c, rx, FTUI_GY(2), tr("Spoor", "Track"));
    ftui_number(c, rx - SX(4), FTUI_GY(5), jstr(t, "track", "-"), 90, changed ? THEME_DANGER : THEME_TEXT_HI);
    if (changed) {
        char was[24];
        snprintf(was, sizeof was, tr("was %s", "was %s"), jstr(t, "planned_track", ""));
        ftui_label_at(c, rx, FTUI_GY(15), was, SF(14), THEME_TEXT_MUTED);
    }
}

static void board(lv_obj_t * body, const jv * first, lv_coord_t y, lv_coord_t w) {
    lv_coord_t h = SY(600) - FTUI_MARGIN - y;
    lv_obj_t * c = ftui_card(body, FTUI_MARGIN, y, w, h, THEME_CARD_BG);
    lv_coord_t x_time = FTUI_G(3), x_dest = SX(176), x_track = w - SX(152);
    ftui_caption(c, x_time, FTUI_GY(2), tr("Tijd", "Time"));
    ftui_caption(c, x_dest, FTUI_GY(2), tr("Richting", "To"));
    ftui_caption(c, x_track, FTUI_GY(2), tr("Spoor", "Track"));

    lv_coord_t row_h = (h - FTUI_GY(5) - FTUI_GY(1)) / BOARD_ROWS;
    lv_coord_t ry = FTUI_GY(5);
    int n = 0;
    for (const jv * t = first; t && n < BOARD_ROWS; t = t->next, n++) {
        int cancelled = jbool(t, "cancelled", 0);
        int delay = (int)jnum(t, "delay", 0);
        if (n > 0) {
            lv_obj_t * d = ftui_plain(c);
            lv_obj_set_pos(d, x_time, ry);
            lv_obj_set_size(d, w - 2 * x_time, 1);
            lv_obj_set_style_bg_color(d, lv_color_hex(THEME_DIVIDER), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        }
        lv_coord_t mid = ry + row_h / 2;
        uint32_t col = cancelled ? THEME_TEXT_MUTED : THEME_TEXT_HI;

        lv_obj_t * tl = ftui_number(c, x_time - SX(2), 0, jstr(t, "time", ""), 30, col);
        lv_obj_set_y(tl, mid - lv_font_get_line_height(ftui_light(30)) / 2);
        if (delay > 0 && !cancelled) {
            char d[12];
            snprintf(d, sizeof d, "+%d", delay);
            lv_obj_t * dl = ftui_label_at(c, x_time + SX(88), 0, d, SF(18), THEME_DANGER);
            lv_obj_set_y(dl, mid - lv_font_get_line_height(SF(18)) / 2);
        }

        lv_obj_t * dest = ftui_label_at(c, x_dest, 0, jstr(t, "dest", "?"), SF(20), col);
        lv_obj_set_y(dest, mid - lv_font_get_line_height(SF(20)) / 2 - SY(1));
        ftui_one_line(dest, SX(380));
        const char * dname = jstr(t, "dest", "?");
        lv_coord_t dw = lv_txt_get_width(dname, (uint32_t)strlen(dname), SF(20), 0, LV_TEXT_FLAG_NONE);
        if (dw > SX(380)) dw = SX(380);
        const char * tag = cancelled ? tr("Rijdt niet", "Cancelled") : jstr(t, "cat", "");
        lv_obj_t * cat = ftui_label_at(c, x_dest + dw + FTUI_G(2), 0, tag, SF(12), cancelled ? THEME_DANGER : THEME_TEXT_MUTED);
        lv_obj_set_style_text_letter_space(cat, 1, 0);
        lv_obj_set_y(cat, mid - lv_font_get_line_height(SF(12)) / 2);

        int changed = jbool(t, "track_changed", 0);
        lv_obj_t * tr_ = ftui_number(c, x_track - SX(2), 0, jstr(t, "track", "-"), 30,
                                     changed && !cancelled ? THEME_DANGER : col);
        lv_obj_set_y(tr_, mid - lv_font_get_line_height(ftui_light(30)) / 2);
        ry += row_h;
    }
    if (n == 0) {
        lv_obj_t * l = ftui_label_at(c, x_dest, FTUI_GY(8), tr("Geen andere treinen", "No other trains"), SF(18), THEME_TEXT_MUTED);
        (void)l;
    }
}

static void build_board(lv_obj_t * body, jv * s) {
    s_walk = (int)jnum(s, "walk_min", 0);
    const char * station = jstr(s, "station", "");
    const char * dir = jstr(s, "direction", "");
    char cap[160];
    if (dir[0]) snprintf(cap, sizeof cap, tr("Vertrektijden  \xe2\x80\xa2  richting %s", "Departures  \xe2\x80\xa2  to %s"), dir);
    else snprintf(cap, sizeof cap, "%s", tr("Vertrektijden", "Departures"));
    ftui_header(body, cap, station[0] ? station : tr("Geen station", "No station"), on_close);
    ftui_icon_button(body, SX(1024) - FTUI_MARGIN - FTUI_BTN, FTUI_HEAD_Y, LV_SYMBOL_SETTINGS, on_settings, 0);

    const char * err = jstr(s, "error", "");
    char foot[160], ago[40];
    long upd = (long)jnum(s, "updated", 0);
    ftui_ago(upd > 0 ? (long)(time(NULL) - upd) : -1, ago, sizeof ago);
    const char * alert = jstr(s, "alert", "");
    int warn = !strncmp(alert, "Let op", 6) || !strncmp(alert, "Note:", 5);
    if (err[0]) snprintf(foot, sizeof foot, "%s", err);
    else if (warn) snprintf(foot, sizeof foot, "%s", alert);
    else if (jbool(s, "mock", 0)) snprintf(foot, sizeof foot, "%s", tr("VOORBEELDGEGEVENS", "SAMPLE DATA"));
    else snprintf(foot, sizeof foot, "%s", ago);
    int sample = jbool(s, "mock", 0) && !err[0] && !warn;
    lv_obj_t * fo = ftui_label(body, foot, SF(12), err[0] || warn ? THEME_DANGER : sample ? THEME_ACCENT : THEME_TEXT_MUTED);
    lv_obj_set_style_text_letter_space(fo, sample ? 2 : 0, 0);
    lv_obj_set_style_text_align(fo, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(fo, SX(420));
    lv_label_set_long_mode(fo, LV_LABEL_LONG_DOT);
    lv_obj_set_height(fo, lv_font_get_line_height(SF(12)));
    lv_obj_set_pos(fo, SX(1024) - FTUI_MARGIN - FTUI_BTN - FTUI_G(2) - SX(420), FTUI_HEAD_Y + SY(22));

    lv_coord_t w = SX(1024) - 2 * FTUI_MARGIN;
    jv * trains = jget(s, "trains");
    jv * first = jfirst(trains);
    if (!first) {
        lv_obj_t * c = ftui_card(body, FTUI_MARGIN, FTUI_TOP, w, FTUI_GY(19), THEME_CARD_BG);
        ftui_caption(c, FTUI_G(3), FTUI_GY(3), err[0] ? tr("Geen gegevens", "No data") : tr("Geen treinen", "No trains"));
        lv_obj_t * t = ftui_label_at(c, FTUI_G(3), FTUI_GY(6),
            err[0] ? err : tr("Er vertrekken nu geen treinen die bij je richting passen.",
                              "No trains matching your direction are leaving now."), SF(22), THEME_TEXT_HI);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, w - FTUI_G(6));
        return;
    }
    hero(body, first, FTUI_TOP, w);
    board(body, first->next, FTUI_TOP + FTUI_GY(19) + FTUI_GAP, w);
}

/* ---- settings ---------------------------------------------------------------- */

static void on_settings_back(lv_event_t * e) { (void)e; s_view = VIEW_BOARD; free(s_state_text); s_state_text = NULL; rebuild(); }

static void on_field_done(const char * key, const char * value) {
    char v[160];
    snprintf(v, sizeof v, "%s", value);
    if (!strcmp(key, "walk_min")) snprintf(v, sizeof v, "%d", atoi(value) < 0 ? 0 : atoi(value) > 60 ? 60 : atoi(value));
    if (!strcmp(key, "warn_delay")) snprintf(v, sizeof v, "%d", atoi(value) < 1 ? 5 : atoi(value) > 120 ? 120 : atoi(value));
    if (ftui_conf_set(CONF_PATH, key, v) != 0) fprintf(stderr, "[ns] saving %s failed\n", CONF_PATH);
    if (!strcmp(key, "station")) { ftui_conf_set(CONF_PATH, "station_code", ""); ftui_conf_set(CONF_PATH, "station_name", ""); }
    ftui_cmd("{\"cmd\":\"refresh\"}");
    rebuild();
}

static void on_field_edit(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= N_FIELDS) return;
    static char cur[1100];
    ftui_conf_get(CONF_PATH, FIELDS[i].key, cur, sizeof cur);
    ftui_prompt(s_root, FIELDS[i].key, tr(FIELDS[i].label_nl, FIELDS[i].label_en),
                tr(FIELDS[i].hint_nl, FIELDS[i].hint_en), cur, FIELDS[i].max_len, on_field_done);
}

static void build_settings(lv_obj_t * body) {
    ftui_header(body, tr("Vertrektijden", "Departures"), tr("Instellingen", "Settings"), on_settings_back);
    lv_obj_t * sc = ftui_scroller(body, FTUI_MARGIN, FTUI_TOP, SX(1024) - 2 * FTUI_MARGIN, SY(600) - FTUI_TOP - FTUI_MARGIN);
    lv_coord_t y = 0, w = SX(1024) - 2 * FTUI_MARGIN - SX(8);
    for (int i = 0; i < N_CONN; i++) y += ftui_field_row(sc, y, w, &FIELDS[i], CONF_PATH, on_field_edit, i) + SY(8);
    y += ftui_section(sc, y, tr("Opties", "Options"));
    y += ftui_toggle_row(sc, y, w, "tile_leave", tr("Vertrektijd op de tegel", "Leave time on the tile"),
                         tr("Toon wanneer je weg moet (vertrek min looptijd) in plaats van de vertrektijd.",
                            "Show when to leave (departure minus walk) instead of the departure."), CONF_PATH) + SY(8);
    for (int i = N_CONN; i < N_FIELDS; i++) y += ftui_field_row(sc, y, w, &FIELDS[i], CONF_PATH, on_field_edit, i) + SY(8);
    char code[16], name[64], line[160];
    ftui_conf_get(CONF_PATH, "station_code", code, sizeof code);
    ftui_conf_get(CONF_PATH, "station_name", name, sizeof name);
    if (code[0]) snprintf(line, sizeof line, tr("Gevonden station: %s (%s). Gegevens: NS Reisinformatie.",
                                                "Station found: %s (%s). Data: NS Reisinformatie."), name, code);
    else snprintf(line, sizeof line, "%s", tr("Gegevens: NS Reisinformatie (apiportal.ns.nl).", "Data: NS Reisinformatie (apiportal.ns.nl)."));
    lv_obj_t * h = ftui_label_at(sc, FTUI_G(1), y + SY(8), line, SF(14), THEME_TEXT_MUTED);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, w - FTUI_G(2));
}

/* ---- lifecycle ---------------------------------------------------------------- */

static void rebuild(void) {
    if (!s_root) return;
    if (s_body) lv_obj_del(s_body);
    s_body = ftui_plain(s_root);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES);
    if (s_view == VIEW_SETTINGS) { build_settings(s_body); return; }
    jv * s = s_state_text ? jparse(s_state_text) : NULL;
    if (s) build_board(s_body, s);
    else {
        ftui_header(s_body, tr("Vertrektijden", "Departures"), tr("Even geduld", "One moment"), on_close);
        ftui_icon_button(s_body, SX(1024) - FTUI_MARGIN - FTUI_BTN, FTUI_HEAD_Y, LV_SYMBOL_SETTINGS, on_settings, 0);
    }
    jfree(s);
}

static void on_tick(lv_timer_t * t) {
    (void)t;
    if (ftui_modal_open_() || lv_scr_act() != s_root || s_view != VIEW_BOARD) return;
    char * text = ftui_read(STATE_PATH);
    if (text && s_state_text && !strcmp(text, s_state_text)) { free(text); return; }
    free(s_state_text);
    s_state_text = text;
    rebuild();
}

static void on_root_deleted(lv_event_t * e) {
    (void)e;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_root = s_body = NULL;
    ftui_modal_ = NULL;
    ftui_prompt_ta_ = NULL;
    free(s_state_text);
    s_state_text = NULL;
}

EXPORT lv_obj_t * screen_create(void) {
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }
    s_view = VIEW_BOARD;
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);
    ftui_cmd("{\"cmd\":\"refresh\"}");              /* fresh departures when the board opens */
    s_state_text = ftui_read(STATE_PATH);
    rebuild();
    s_timer = lv_timer_create(on_tick, 2000, NULL);
    return s_root;
}
