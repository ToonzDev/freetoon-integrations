/*
 * screen_homey.c - FreeToon UI plugin (homey_ui.<toon>.so): start Homey
 * flows and see / switch a few devices.
 *
 * Main view: the chosen flows as calm buttons with an icon (tap = start it),
 * the chosen devices on the right with their state and an on/off switch.
 * Settings: Homey address and token, and two pickers that list every flow
 * Homey can start and every device (from the daemon's flows_all.json /
 * devices_all.json); tapping a row adds or removes it.
 *
 * Talks to the daemon only through files: state.json in, cmd-*.json out.
 */
#define _GNU_SOURCE
#define FTUI_ID "homey"
#include "ftui.h"
#include "ftjson.h"
#include "plugin_abi.h"

FT_PLUGIN_DECLARE_ABI(
    FT_SETTINGS_FIELD(language),
    FT_SETTINGS_FIELD(home_light_mode));

#define EXPORT __attribute__((visibility("default")))

#define STATE_PATH   FTUI_RUN "/state.json"
#define FLOWS_PATH   FTUI_RUN "/flows_all.json"
#define DEVICES_PATH FTUI_RUN "/devices_all.json"
#define CONF_PATH    FTUI_DIR "/homey.conf"
#define MAX_PICK     64

enum { VIEW_MAIN = 0, VIEW_SETTINGS, VIEW_PICK_FLOWS, VIEW_PICK_DEVICES, VIEW_PICK_ONE };

static lv_obj_t *   s_root;
static lv_obj_t *   s_body;
static lv_obj_t *   s_flow_sc, * s_dev_sc;  /* main view scrollers (positions survive a refresh) */
static lv_timer_t * s_timer;
static int          s_view;
static char *       s_state_text;
static char         s_pending[48];      /* flow just tapped, until the daemon answers */
static time_t       s_pending_at;
static char         s_pick_ids[MAX_PICK][48];
static const char * s_one_key;          /* VIEW_PICK_ONE: config key that gets one flow id */
static const char * s_one_title;

/* Automatic starts: Toon program state -> flow, and two times of day. */
static const struct { const char * key; const char * nl; const char * en; } RULES[] = {
    { "on_away",    "Bij Weg",     "On Away" },
    { "on_home",    "Bij Thuis",   "On Home" },
    { "on_sleep",   "Bij Slapen",  "On Sleep" },
    { "on_comfort", "Bij Comfort", "On Comfort" },
    { "at1_flow",   "Flow op tijd 1", "Flow at time 1" },
    { "at2_flow",   "Flow op tijd 2", "Flow at time 2" },
};
#define N_RULES ((int)(sizeof RULES / sizeof RULES[0]))

static const ftui_field_t FIELDS[] = {
    { "host", "Homey-adres", "Homey address",
      "Het IP-adres van de Homey in je netwerk, bijvoorbeeld 192.168.1.30.",
      "The Homey's IP address on your network, e.g. 192.168.1.30.", 0, 60 },
    { "token", "Toegangstoken", "Access token",
      "Homey Pro: Instellingen > API-sleutels. Oudere Homey: zie README (sessietoken).",
      "Homey Pro: Settings > API keys. Older Homey: see README (session token).", 1, 1090 },
    { "homey_id", "Homey-id", "Homey id",
      "Alleen oudere Homey: wordt ingevuld door tools/homey-login.sh.",
      "Older Homey only: filled in by tools/homey-login.sh.", 0, 30 },
    { "cloud_refresh", "Vernieuwsleutel", "Renewal key",
      "Alleen oudere Homey: de app vernieuwt hiermee elke dag zelf het token.",
      "Older Homey only: the app uses it to renew the token every day.", 1, 500 },
};
#define N_CONN 2                                   /* address + token; the rest under "Oudere Homey" */
#define N_FIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static void rebuild(void);
static void on_close(lv_event_t * e) { (void)e; ui_pop(); }
static void on_settings(lv_event_t * e) { (void)e; s_view = VIEW_SETTINGS; rebuild(); }

/* ---- icons --------------------------------------------------------------------- */

static int has(const char * name, const char * words) {
    char w[160];
    snprintf(w, sizeof w, "%s", words);
    for (char * t = strtok(w, "|"); t; t = strtok(NULL, "|")) {
        size_t n = strlen(t);
        for (const char * p = name; *p; p++) if (!strncasecmp(p, t, n)) return 1;
    }
    return 0;
}

/* LVGL has no lamp or bed glyph; pick the closest calm symbol from the name. */
static const char * flow_icon(const char * name) {
    if (has(name, "film|tv|movie|kijk|netflix")) return LV_SYMBOL_VIDEO;
    if (has(name, "muziek|music|sonos|radio|speaker")) return LV_SYMBOL_AUDIO;
    if (has(name, "game|gaming|playstation|xbox")) return LV_SYMBOL_PLAY;
    if (has(name, "slaap|slapen|nacht|sleep|night|bed")) return LV_SYMBOL_EYE_CLOSE;
    if (has(name, "weg|away|vertrek|leave")) return LV_SYMBOL_HOME;
    if (has(name, "thuis|home|welkom")) return LV_SYMBOL_HOME;
    if (has(name, "wekker|alarm|wake")) return LV_SYMBOL_BELL;
    if (has(name, "licht|lamp|light|lampen")) return LV_SYMBOL_EYE_OPEN;
    if (has(name, "stroom|power|aan|uit|on|off")) return LV_SYMBOL_POWER;
    if (has(name, "download|update")) return LV_SYMBOL_DOWNLOAD;
    if (has(name, "restart|herstart|reset")) return LV_SYMBOL_REFRESH;
    return LV_SYMBOL_PLAY;
}

/* ---- main view ------------------------------------------------------------------ */

static void on_flow(lv_event_t * e) {
    lv_obj_t * b = lv_event_get_target(e);
    const char * id = (const char *)lv_obj_get_user_data(b);
    int adv = (int)(intptr_t)lv_event_get_user_data(e);
    if (!id || !id[0]) return;
    /* a second tap while the first is still on its way does nothing */
    if (s_pending[0] && time(NULL) - s_pending_at < 15) return;
    char cmd[160];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"trigger\",\"id\":\"%s\",\"adv\":%d}", id, adv);
    ftui_cmd(cmd);
    snprintf(s_pending, sizeof s_pending, "%s", id);
    s_pending_at = time(NULL);
    rebuild();
}

static void on_switch(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    const char * id = (const char *)lv_obj_get_user_data(sw);
    if (!id || !id[0]) return;
    char cmd[160];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"set\",\"id\":\"%s\",\"cap\":\"onoff\",\"value\":%s}", id,
             lv_obj_has_state(sw, LV_STATE_CHECKED) ? "true" : "false");
    ftui_cmd(cmd);
}

/* Strings handed to LVGL user_data must outlive the widgets: keep them here. */
static char s_ids[2 * MAX_PICK][48];
static int  s_n_ids;
static const char * keep_id(const char * id) {
    if (s_n_ids >= 2 * MAX_PICK) return NULL;
    snprintf(s_ids[s_n_ids], sizeof s_ids[0], "%s", id);
    return s_ids[s_n_ids++];
}

static void flow_button(lv_obj_t * parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, const jv * f,
                        const jv * last) {
    const char * id = jstr(f, "id", "");
    const char * name = jstr(f, "name", "?");
    int enabled = jbool(f, "enabled", 1);
    int pending = s_pending[0] && !strcmp(s_pending, id) && time(NULL) - s_pending_at < 15;
    int was_last = !strcmp(jstr(last, "id", ""), id) && time(NULL) - (time_t)jnum(last, "at", 0) < 20;
    int ok = (int)jnum(last, "ok", -1);

    lv_obj_t * b = ftui_button(parent, x, y, w, h, pending ? THEME_ACCENT_SOFT : THEME_CARD_BG, THEME_TEXT_HI,
                               NULL, NULL, on_flow, jnum(f, "adv", 0) ? 1 : 0);
    lv_obj_set_style_radius(b, THEME_RADIUS_TILE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_ACCENT_SOFT), LV_STATE_PRESSED);
    lv_obj_set_user_data(b, (void *)keep_id(id));

    lv_coord_t d = SX(48);
    lv_obj_t * ring = ftui_dot(b, FTUI_G(3), (h - d) / 2, d, pending || (was_last && ok == 1) ? THEME_ACCENT_FILL : THEME_ROW_BG);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * ic = ftui_label(ring, was_last && ok == 1 ? LV_SYMBOL_OK : flow_icon(name), SF(20),
                               pending || (was_last && ok == 1) ? THEME_ON_ACCENT : enabled ? THEME_ACCENT : THEME_TEXT_MUTED);
    lv_obj_center(ic);

    lv_coord_t tx = FTUI_G(3) + d + FTUI_G(2);
    lv_obj_t * nl = ftui_label_at(b, tx, h / 2 - SY(20), name, SF(20), enabled ? THEME_TEXT_HI : THEME_TEXT_MUTED);
    ftui_one_line(nl, w - tx - FTUI_G(2));
    const char * sub = pending ? tr("Wordt gestart", "Starting")
                     : was_last ? (ok == 1 ? tr("Gestart", "Started") : tr("Niet gelukt", "Failed"))
                     : !enabled ? tr("Staat uit in Homey", "Disabled in Homey")
                     : jnum(f, "adv", 0) ? tr("Geavanceerde flow", "Advanced flow") : "Flow";
    lv_obj_t * sl = ftui_label_at(b, tx, h / 2 + SY(4), sub, SF(12),
                                  was_last && ok == 0 ? THEME_DANGER : pending || (was_last && ok == 1) ? THEME_ACCENT : THEME_TEXT_MUTED);
    lv_obj_set_style_text_letter_space(sl, 1, 0);
    ftui_one_line(sl, w - tx - FTUI_G(2));
}

static void device_row(lv_obj_t * parent, lv_coord_t y, lv_coord_t w, lv_coord_t h, const jv * d) {
    int on = (int)jnum(d, "on", -1), avail = jbool(d, "available", 1);
    lv_obj_t * row = ftui_plain(parent);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, w, h);
    lv_obj_t * nl = ftui_label_at(row, FTUI_G(3), h / 2 - SY(20), jstr(d, "name", "?"), SF(18), avail ? THEME_TEXT_HI : THEME_TEXT_MUTED);
    ftui_one_line(nl, w - SX(120) - FTUI_G(3));
    char sub[80];
    double p = jnum(d, "power", -1), t = jnum(d, "temp", -999);
    const char * zone = jstr(d, "zone", "");
    if (!avail) snprintf(sub, sizeof sub, "%s", tr("Niet bereikbaar", "Unavailable"));
    else if (p >= 0) snprintf(sub, sizeof sub, "%s%s%.0f W", zone, zone[0] ? "  \xe2\x80\xa2  " : "", p);
    else if (t > -900) snprintf(sub, sizeof sub, "%s%s%.1f\xc2\xb0", zone, zone[0] ? "  \xe2\x80\xa2  " : "", t);
    else snprintf(sub, sizeof sub, "%s", zone);
    ftui_label_at(row, FTUI_G(3), h / 2 + SY(2), sub, SF(12), avail ? THEME_TEXT_DIM : THEME_DANGER);

    if (on >= 0) {
        lv_obj_t * sw = lv_switch_create(row);
        lv_obj_set_size(sw, SX(56), SY(32));
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -FTUI_G(3), 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_OFF), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_ACCENT_FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(THEME_ON_ACCENT), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sw, -SX(3), LV_PART_KNOB);
        lv_obj_set_style_border_width(sw, 0, 0);
        lv_obj_set_ext_click_area(sw, SX(12));
        if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
        if (!avail) lv_obj_add_state(sw, LV_STATE_DISABLED);
        lv_obj_set_user_data(sw, (void *)keep_id(jstr(d, "id", "")));
        lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void on_pick_flows(lv_event_t * e) { (void)e; s_view = VIEW_PICK_FLOWS; rebuild(); }

static void build_main(lv_obj_t * body, jv * s) {
    s_n_ids = 0;
    ftui_header(body, "Homey", tr("Flows en apparaten", "Flows and devices"), on_close);
    ftui_icon_button(body, SX(1024) - FTUI_MARGIN - FTUI_BTN, FTUI_HEAD_Y, LV_SYMBOL_SETTINGS, on_settings, 0);
    const char * err = jstr(s, "error", "");
    if (err[0] || !jbool(s, "configured", 0)) {
        lv_obj_t * fo = ftui_label(body, err[0] ? err : tr("Nog niet ingesteld", "Not set up yet"), SF(12), THEME_DANGER);
        lv_obj_set_style_text_align(fo, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(fo, SX(360));
        lv_obj_set_pos(fo, SX(1024) - FTUI_MARGIN - FTUI_BTN - FTUI_G(2) - SX(360), FTUI_HEAD_Y + SY(22));
    }

    jv * flows = jget(s, "flows");
    jv * devs = jget(s, "devices");
    jv * last = jget(s, "last");
    int nd = jcount(devs);
    lv_coord_t right_w = nd ? SX(328) : 0;
    lv_coord_t left_w = SX(1024) - 2 * FTUI_MARGIN - (nd ? right_w + FTUI_GAP : 0);
    lv_coord_t area_h = SY(600) - FTUI_TOP - FTUI_MARGIN;

    if (!jcount(flows)) {
        lv_obj_t * c = ftui_card(body, FTUI_MARGIN, FTUI_TOP, left_w, FTUI_GY(22), THEME_CARD_BG);
        ftui_caption(c, FTUI_G(4), FTUI_GY(4), tr("Nog geen flows gekozen", "No flows chosen yet"));
        lv_obj_t * t = ftui_label_at(c, FTUI_G(4), FTUI_GY(7),
            tr("Kies de flows die je vanaf de Toon wilt starten.", "Choose the flows you want to start from the Toon."),
            SF(20), THEME_TEXT_HI);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, left_w - FTUI_G(8));
        ftui_button(c, FTUI_G(4), FTUI_GY(14), SX(220), FTUI_BTN, THEME_ACCENT_FILL, THEME_ON_ACCENT,
                    tr("Flows kiezen", "Choose flows"), SF(18), on_pick_flows, 0);
    } else {
        lv_obj_t * sc = ftui_scroller(body, FTUI_MARGIN, FTUI_TOP, left_w, area_h);
        s_flow_sc = sc;
        int cols = left_w > SX(800) ? 3 : 2;
        lv_coord_t bw = (left_w - FTUI_GAP * (cols - 1)) / cols, bh = SY(104);
        int i = 0;
        for (jv * f = jfirst(flows); f; f = f->next, i++)
            flow_button(sc, (i % cols) * (bw + FTUI_GAP), (i / cols) * (bh + FTUI_GAP), bw, bh, f, last);
    }

    if (nd) {
        lv_coord_t x = SX(1024) - FTUI_MARGIN - right_w;
        lv_coord_t row_h = FTUI_GY(9);
        lv_coord_t h = FTUI_GY(6) + nd * row_h + FTUI_GY(1);
        if (h > area_h) h = area_h;
        lv_obj_t * c = ftui_card(body, x, FTUI_TOP, right_w, h, THEME_CARD_BG);
        ftui_caption(c, FTUI_G(3), FTUI_GY(3), tr("Apparaten", "Devices"));
        lv_obj_t * sc = ftui_scroller(c, 0, FTUI_GY(6), right_w, h - FTUI_GY(6));
        s_dev_sc = sc;
        int i = 0;
        for (jv * d = jfirst(devs); d; d = d->next, i++) {
            if (i) {
                lv_obj_t * dv = ftui_plain(sc);
                lv_obj_set_pos(dv, FTUI_G(3), i * row_h);
                lv_obj_set_size(dv, right_w - FTUI_G(6), 1);
                lv_obj_set_style_bg_color(dv, lv_color_hex(THEME_DIVIDER), 0);
                lv_obj_set_style_bg_opa(dv, LV_OPA_COVER, 0);
            }
            device_row(sc, i * row_h, right_w, row_h, d);
        }
    }
}

/* ---- pickers -------------------------------------------------------------------- */

/* Homey flow/device ids: letters, digits and '-', at most 40 (same rule as the daemon) */
static int homey_id_ok(const char * s) {
    if (!s || !*s || strlen(s) > 40) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-') return 0;
    return 1;
}

static int list_has(const char * list, const char * id) {
    size_t n = strlen(id);
    if (!n) return 0;
    for (const char * p = list; (p = strstr(p, id)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

static void toggle_in_conf(const char * key, const char * id) {
    char cur[MAX_PICK * 40], out[MAX_PICK * 40] = "";
    ftui_conf_get(CONF_PATH, key, cur, sizeof cur);
    int found = 0;
    for (char * t = strtok(cur, ","); t; t = strtok(NULL, ",")) {
        if (!strcmp(t, id)) { found = 1; continue; }
        size_t l = strlen(out);
        snprintf(out + l, sizeof out - l, "%s%s", l ? "," : "", t);
    }
    if (!found) { size_t l = strlen(out); snprintf(out + l, sizeof out - l, "%s%s", l ? "," : "", id); }
    ftui_conf_set(CONF_PATH, key, out);
}

static void on_pick_row(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_view == VIEW_PICK_ONE) {                  /* -1 = "no flow" */
        ftui_conf_set(CONF_PATH, s_one_key, i >= 0 && i < MAX_PICK ? s_pick_ids[i] : "");
        s_view = VIEW_SETTINGS;
        ftui_cmd("{\"cmd\":\"refresh\"}");
        rebuild();
        return;
    }
    if (i < 0 || i >= MAX_PICK) return;
    toggle_in_conf(s_view == VIEW_PICK_FLOWS ? "flows" : "devices", s_pick_ids[i]);
    rebuild();
}

static void on_pick_done(lv_event_t * e) { (void)e; s_view = VIEW_SETTINGS; ftui_cmd("{\"cmd\":\"refresh\"}"); rebuild(); }

static void on_rule(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= N_RULES) return;
    s_one_key = RULES[i].key;
    s_one_title = tr(RULES[i].nl, RULES[i].en);
    s_view = VIEW_PICK_ONE;
    rebuild();
}

static void build_picker(lv_obj_t * body, int flows) {
    int one = s_view == VIEW_PICK_ONE;
    ftui_header(body, "Homey", one ? s_one_title : flows ? tr("Flows kiezen", "Choose flows") : tr("Apparaten kiezen", "Choose devices"),
                on_pick_done);
    char * text = ftui_read(flows ? FLOWS_PATH : DEVICES_PATH);
    jv * all = text ? jparse(text) : NULL;
    free(text);
    char sel[MAX_PICK * 40];
    ftui_conf_get(CONF_PATH, one ? s_one_key : flows ? "flows" : "devices", sel, sizeof sel);

    lv_coord_t w = SX(1024) - 2 * FTUI_MARGIN;
    lv_obj_t * sc = ftui_scroller(body, FTUI_MARGIN, FTUI_TOP, w, SY(600) - FTUI_TOP - FTUI_MARGIN);
    if (!jcount(all)) {
        lv_obj_t * t = ftui_label_at(sc, 0, FTUI_GY(1),
            tr("Nog geen lijst van de Homey. Controleer adres en token; de lijst komt binnen een minuut.",
               "No list from the Homey yet. Check address and token; the list arrives within a minute."), SF(18), THEME_TEXT_DIM);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, w - FTUI_G(2));
    }
    lv_coord_t y = 0, rh = FTUI_GY(8), rw = w - SX(8);
    if (one) {
        lv_obj_t * row = ftui_button(sc, 0, y, rw, rh, sel[0] ? THEME_CARD_BG : THEME_ACCENT_SOFT, THEME_TEXT_HI, NULL, NULL, on_pick_row, -1);
        lv_obj_set_style_radius(row, THEME_RADIUS_TILE, 0);
        lv_obj_t * nl = ftui_label(row, tr("Geen flow (uit)", "No flow (off)"), SF(20), THEME_TEXT_DIM);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, FTUI_G(8), 0);
        y += rh + SY(8);
    }
    int i = 0;
    for (jv * it = jfirst(all); it && i < MAX_PICK; it = it->next, i++) {
        const char * pid = jstr(it, "id", "");
        if (!homey_id_ok(pid)) { i--; continue; }   /* only plain Homey ids get stored */
        snprintf(s_pick_ids[i], sizeof s_pick_ids[0], "%s", pid);
        int on = list_has(sel, s_pick_ids[i]);
        lv_obj_t * row = ftui_button(sc, 0, y, rw, rh, on ? THEME_ACCENT_SOFT : THEME_CARD_BG, THEME_TEXT_HI, NULL, NULL, on_pick_row, i);
        lv_obj_set_style_radius(row, THEME_RADIUS_TILE, 0);
        const char * name = jstr(it, "name", "?");
        lv_obj_t * ic = ftui_label(row, flows ? flow_icon(name) : LV_SYMBOL_POWER, SF(18), on ? THEME_ACCENT : THEME_TEXT_MUTED);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, FTUI_G(3), 0);
        lv_obj_t * nl = ftui_label(row, name, SF(20), THEME_TEXT_HI);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, FTUI_G(8), 0);
        char meta[80];
        if (flows) snprintf(meta, sizeof meta, "%s%s", jnum(it, "adv", 0) ? tr("Geavanceerd", "Advanced") : "Flow",
                            jbool(it, "enabled", 1) ? "" : tr("  \xe2\x80\xa2  staat uit", "  \xe2\x80\xa2  disabled"));
        else snprintf(meta, sizeof meta, "%s", jstr(it, "zone", ""));
        lv_obj_t * ml = ftui_label(row, meta, SF(12), THEME_TEXT_MUTED);
        lv_obj_set_style_text_letter_space(ml, 1, 0);
        lv_obj_align(ml, LV_ALIGN_RIGHT_MID, -FTUI_G(8), 0);
        if (on) {
            lv_obj_t * ok = ftui_label(row, LV_SYMBOL_OK, SF(18), THEME_ACCENT);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -FTUI_G(3), 0);
        }
        y += rh + SY(8);
    }
    jfree(all);
}

/* ---- settings --------------------------------------------------------------------- */

static void on_settings_back(lv_event_t * e) { (void)e; s_view = VIEW_MAIN; free(s_state_text); s_state_text = NULL; rebuild(); }
static void on_pick_devices(lv_event_t * e) { (void)e; s_view = VIEW_PICK_DEVICES; rebuild(); }

static void on_field_done(const char * key, const char * value) {
    if (ftui_conf_set(CONF_PATH, key, value) != 0) fprintf(stderr, "[homey] saving %s failed\n", CONF_PATH);
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

static void on_time_done(const char * key, const char * value) {
    int h, m;
    char v[8] = "";
    if (sscanf(value, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) snprintf(v, sizeof v, "%02d:%02d", h, m);
    ftui_conf_set(CONF_PATH, key, v);
    ftui_cmd("{\"cmd\":\"refresh\"}");
    rebuild();
}

static void on_time_edit(lv_event_t * e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    const char * key = k ? "at2_time" : "at1_time";
    char cur[16];
    ftui_conf_get(CONF_PATH, key, cur, sizeof cur);
    ftui_prompt(s_root, key, k ? tr("Tijd 2", "Time 2") : tr("Tijd 1", "Time 1"),
                tr("Uren:minuten, bijvoorbeeld 07:00. Leeg = uit.", "Hours:minutes, e.g. 07:00. Empty = off."), cur, 5, on_time_done);
}

static lv_coord_t pick_row(lv_obj_t * sc, lv_coord_t y, lv_coord_t w, const char * cap, const char * key, lv_event_cb_t cb) {
    char sel[MAX_PICK * 40], val[64];
    ftui_conf_get(CONF_PATH, key, sel, sizeof sel);
    int n = 0;
    for (char * p = sel; *p; p++) if (*p == ',') n++;
    if (sel[0]) n++;
    if (n) snprintf(val, sizeof val, tr("%d gekozen", "%d chosen"), n);
    else snprintf(val, sizeof val, "%s", tr("Nog niets gekozen", "Nothing chosen yet"));
    lv_coord_t h = FTUI_GY(10);
    lv_obj_t * row = ftui_button(sc, 0, y, w, h, THEME_CARD_BG, THEME_TEXT_HI, NULL, NULL, cb, 0);
    lv_obj_set_style_radius(row, THEME_RADIUS_TILE, 0);
    ftui_caption(row, FTUI_G(3), FTUI_GY(2), cap);
    ftui_label_at(row, FTUI_G(3), FTUI_GY(4) + SY(4), val, SF(20), n ? THEME_TEXT_HI : THEME_TEXT_MUTED);
    lv_obj_t * ch = ftui_label(row, LV_SYMBOL_RIGHT, SF(18), THEME_TEXT_MUTED);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -FTUI_G(3), 0);
    return h;
}

static void build_settings(lv_obj_t * body) {
    ftui_header(body, "Homey", tr("Instellingen", "Settings"), on_settings_back);
    lv_obj_t * sc = ftui_scroller(body, FTUI_MARGIN, FTUI_TOP, SX(1024) - 2 * FTUI_MARGIN, SY(600) - FTUI_TOP - FTUI_MARGIN);
    lv_coord_t y = 0, w = SX(1024) - 2 * FTUI_MARGIN - SX(8);
    for (int i = 0; i < N_CONN; i++) y += ftui_field_row(sc, y, w, &FIELDS[i], CONF_PATH, on_field_edit, i) + SY(8);
    y += pick_row(sc, y, w, tr("Flows op de Toon", "Flows on the Toon"), "flows", on_pick_flows) + SY(8);
    y += pick_row(sc, y, w, tr("Apparaten op de Toon", "Devices on the Toon"), "devices", on_pick_devices) + SY(8);

    /* automatic starts */
    y += ftui_section(sc, y, tr("Automatisch starten", "Start automatically"));
    y += ftui_toggle_row(sc, y, w, "auto", tr("Regels gebruiken", "Use the rules"),
                         tr("Start een flow als de Toon van programma wisselt of op een vaste tijd.",
                            "Start a flow when the Toon changes program or at a set time."), CONF_PATH) + SY(8);
    char * text = ftui_read(FLOWS_PATH);
    jv * all = text ? jparse(text) : NULL;
    free(text);
    for (int i = 0; i < N_RULES; i++) {
        if (i == 4) {
            for (int k = 0; k < 2; k++) {
                static const ftui_field_t TF[2] = {
                    { "at1_time", "Tijd 1", "Time 1", "Bijvoorbeeld 07:00. Leeg = uit.", "For example 07:00. Empty = off.", 0, 5 },
                    { "at2_time", "Tijd 2", "Time 2", "Bijvoorbeeld 22:30. Leeg = uit.", "For example 22:30. Empty = off.", 0, 5 },
                };
                y += ftui_field_row(sc, y, w, &TF[k], CONF_PATH, on_time_edit, k) + SY(8);
            }
        }
        char id[48], val[96];
        ftui_conf_get(CONF_PATH, RULES[i].key, id, sizeof id);
        const char * name = NULL;
        for (jv * f = jfirst(all); f && id[0]; f = f->next) if (!strcmp(jstr(f, "id", ""), id)) name = jstr(f, "name", NULL);
        snprintf(val, sizeof val, "%s", id[0] ? (name ? name : tr("Onbekende flow", "Unknown flow")) : tr("Geen", "None"));
        lv_coord_t h = FTUI_GY(10);
        lv_obj_t * row = ftui_button(sc, 0, y, w, h, THEME_CARD_BG, THEME_TEXT_HI, NULL, NULL, on_rule, i);
        lv_obj_set_style_radius(row, THEME_RADIUS_TILE, 0);
        ftui_caption(row, FTUI_G(3), FTUI_GY(2), tr(RULES[i].nl, RULES[i].en));
        ftui_label_at(row, FTUI_G(3), FTUI_GY(4) + SY(4), val, SF(20), id[0] ? THEME_TEXT_HI : THEME_TEXT_MUTED);
        lv_obj_t * ch = ftui_label(row, LV_SYMBOL_RIGHT, SF(18), THEME_TEXT_MUTED);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -FTUI_G(3), 0);
        y += h + SY(8);
    }
    jfree(all);

    /* older Homey: automatic token renewal */
    y += ftui_section(sc, y, tr("Oudere Homey (zonder API-sleutels)", "Older Homey (no API keys)"));
    for (int i = N_CONN; i < N_FIELDS; i++) y += ftui_field_row(sc, y, w, &FIELDS[i], CONF_PATH, on_field_edit, i) + SY(8);

    lv_obj_t * h = ftui_label_at(sc, FTUI_G(1), y + SY(8),
        tr("Programma-regels reageren op een knop op de Toon en op het weekschema. Alles staat standaard uit.",
           "Program rules react to a button on the Toon and to the weekly schedule. Everything is off by default."),
        SF(14), THEME_TEXT_MUTED);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, w - FTUI_G(2));
}

/* ---- lifecycle ------------------------------------------------------------------------ */

static void rebuild(void) {
    if (!s_root) return;
    lv_coord_t fy = s_flow_sc ? lv_obj_get_scroll_y(s_flow_sc) : 0, dy = s_dev_sc ? lv_obj_get_scroll_y(s_dev_sc) : 0;
    int was_main = s_flow_sc || s_dev_sc;
    s_flow_sc = s_dev_sc = NULL;
    if (s_body) lv_obj_del(s_body);
    s_body = ftui_plain(s_root);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES);
    if (s_view == VIEW_SETTINGS) { build_settings(s_body); return; }
    if (s_view == VIEW_PICK_FLOWS || s_view == VIEW_PICK_DEVICES || s_view == VIEW_PICK_ONE) {
        build_picker(s_body, s_view != VIEW_PICK_DEVICES);
        return;
    }
    jv * s = s_state_text ? jparse(s_state_text) : NULL;
    if (!s) s = jparse("{}");
    build_main(s_body, s);
    jfree(s);
    if (was_main) {
        if (s_flow_sc && fy) { lv_obj_update_layout(s_flow_sc); lv_obj_scroll_to_y(s_flow_sc, fy, LV_ANIM_OFF); }
        if (s_dev_sc && dy) { lv_obj_update_layout(s_dev_sc); lv_obj_scroll_to_y(s_dev_sc, dy, LV_ANIM_OFF); }
    }
}

static void on_tick(lv_timer_t * t) {
    (void)t;
    if (ftui_modal_open_() || lv_scr_act() != s_root || s_view != VIEW_MAIN) return;
    /* never rebuild under a finger (a scroll or a tap in progress) */
    lv_indev_t * in = lv_indev_get_next(NULL);
    for (; in; in = lv_indev_get_next(in))
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && in->proc.state == LV_INDEV_STATE_PRESSED) return;
    char * text = ftui_read(STATE_PATH);
    if (text && s_state_text && !strcmp(text, s_state_text)) { free(text); return; }
    free(s_state_text);
    s_state_text = text;
    /* the daemon answered for the flow we started: stop showing "Starting" */
    jv * s = text ? jparse(text) : NULL;
    if (s && s_pending[0] && !strcmp(jstr(s, "last.id", ""), s_pending) && jnum(s, "last.at", 0) >= (double)s_pending_at)
        s_pending[0] = 0;
    jfree(s);
    rebuild();
}

static void on_root_deleted(lv_event_t * e) {
    (void)e;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_root = s_body = NULL;
    s_flow_sc = s_dev_sc = NULL;
    ftui_modal_ = NULL;
    ftui_prompt_ta_ = NULL;
    free(s_state_text);
    s_state_text = NULL;
}

EXPORT lv_obj_t * screen_create(void) {
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }
    s_view = VIEW_MAIN;
    s_pending[0] = 0;
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);
    ftui_cmd("{\"cmd\":\"refresh\"}");
    s_state_text = ftui_read(STATE_PATH);
    rebuild();
    s_timer = lv_timer_create(on_tick, 1000, NULL);
    return s_root;
}
