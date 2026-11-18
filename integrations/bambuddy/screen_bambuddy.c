/*
 * screen_bambuddy.c - FreeToon UI plugin (bambuddy_ui.<toon>.so): the
 * 3D-printer screen. toonui dlopen()s it when the tile is tapped.
 *
 * Reads what the daemon writes to /var/volatile/tmp/bambuddy/: state.json
 * (normalised printer status) and cover.png (plate thumbnail). Shows the
 * thumbnail, file, progress, time left and finish time, layers, nozzle / bed
 * / chamber temperatures and the AMS spools. A settings view edits
 * /mnt/data/integrations/bambuddy/bambuddy.conf (BamBuddy URL, API key,
 * printer, push token); the daemon picks changes up within a second.
 */
#define _GNU_SOURCE
#define FTUI_ID "bambuddy"
#include "ftui.h"
#include "ftjson.h"
#include "plugin_abi.h"

#include <math.h>

/* tr() reads `language`; THEME_* falls back to `home_light_mode`. */
FT_PLUGIN_DECLARE_ABI(
    FT_SETTINGS_FIELD(language),
    FT_SETTINGS_FIELD(home_light_mode));

#define EXPORT __attribute__((visibility("default")))

#define STATE_PATH FTUI_RUN "/state.json"
#define COVER_PATH FTUI_RUN "/cover.png"
#define CONF_PATH  FTUI_DIR "/bambuddy.conf"
#define COVER_PX   SY(152)

enum { VIEW_STATUS = 0, VIEW_SETTINGS = 1 };

static lv_obj_t *   s_root;
static lv_obj_t *   s_body;
static lv_timer_t * s_timer;
static int          s_view;
static char *       s_state_text;       /* last state.json, to skip identical redraws */
static lv_img_dsc_t * s_cover;          /* decoded + scaled thumbnail */
static long         s_cover_mtime;

static const ftui_field_t FIELDS[] = {
    { "url", "BamBuddy-adres", "BamBuddy address",
      "Zoals de Toon het bereikt, bv. http://192.168.1.20:8000. Leeg = alleen push.",
      "As the Toon reaches it, e.g. http://192.168.1.20:8000. Empty = push only.", 0, 150 },
    { "api_key", "API-sleutel", "API key",
      "BamBuddy > Instellingen > API-sleutels (alleen lezen is genoeg).",
      "BamBuddy > Settings > API keys (read-only is enough).", 1, 90 },
    { "printer_id", "Printer-nummer", "Printer number",
      "0 = de eerste printer in BamBuddy.", "0 = the first printer in BamBuddy.", 0, 4 },
    { "push_port", "Push-poort", "Push port",
      "Poort voor een relay, bv. 8766. 0 = uit (alleen pull).", "Port for a relay, e.g. 8766. 0 = off (pull only).", 0, 5 },
    { "push_token", "Push-geheim", "Push secret",
      "Nodig voor een relay (relay/bambuddy-relay.sh); minstens 8 tekens.",
      "Needed for a relay (relay/bambuddy-relay.sh); at least 8 characters.", 1, 60 },
    { "push_from", "Adres van de relay", "Relay address",
      "Alleen dit IPv4-adres mag pushen, bv. 192.168.1.20. Leeg = elk adres.",
      "Only this IPv4 address may push, e.g. 192.168.1.20. Empty = any address.", 0, 40 },
};
#define N_FIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static void rebuild(void);

/* ---- thumbnail ----------------------------------------------------------- */

static void cover_free(void) {
    if (s_cover) { lv_img_buf_free(s_cover); s_cover = NULL; }
}

/* Decode cover.png once (LVGL's image cache is off on the Toon, so an lv_img
 * on the file would decode the PNG on every redraw) and box-scale it into a
 * COVER_PX square with alpha. */
static void cover_load(void) {
    long mt = 0;
    struct stat st;
    if (stat(COVER_PATH, &st) == 0) mt = (long)st.st_mtime ^ (long)st.st_size;
    if (mt == s_cover_mtime) return;               /* also after a failed decode: no retry loop */
    s_cover_mtime = mt;
    cover_free();
    if (!mt) return;

    lv_img_decoder_dsc_t dsc;
    memset(&dsc, 0, sizeof dsc);
    if (lv_img_decoder_open(&dsc, "S:" COVER_PATH, lv_color_white(), 0) != LV_RES_OK) return;
    if (!dsc.img_data || dsc.header.w == 0 || dsc.header.h == 0) { lv_img_decoder_close(&dsc); return; }

    lv_img_dsc_t src;
    memset(&src, 0, sizeof src);
    src.header = dsc.header;
    src.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    src.data = dsc.img_data;
    int sw = dsc.header.w, sh = dsc.header.h;
    int size = COVER_PX;
    int dw = size, dh = size;
    if (sw > sh) dh = size * sh / sw; else dw = size * sw / sh;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    lv_img_dsc_t * out = lv_img_buf_alloc((lv_coord_t)dw, (lv_coord_t)dh, LV_IMG_CF_TRUE_COLOR_ALPHA);
    if (out) {
        for (int y = 0; y < dh; y++) {
            int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
                if (x1 <= x0) x1 = x0 + 1;
                unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int yy = y0; yy < y1; yy++) {
                    for (int xx = x0; xx < x1; xx++) {
                        lv_opa_t al = lv_img_buf_get_px_alpha(&src, (lv_coord_t)xx, (lv_coord_t)yy);
                        uint32_t c32 = lv_color_to32(lv_img_buf_get_px_color(&src, (lv_coord_t)xx, (lv_coord_t)yy, lv_color_white()));
                        /* premultiply so transparent pixels don't bleed their colour */
                        r += ((c32 >> 16) & 0xff) * al;
                        g += ((c32 >> 8) & 0xff) * al;
                        b += (c32 & 0xff) * al;
                        a += al;
                        n++;
                    }
                }
                lv_color_t c = a ? lv_color_make((uint8_t)(r / a), (uint8_t)(g / a), (uint8_t)(b / a)) : lv_color_black();
                lv_img_buf_set_px_color(out, (lv_coord_t)x, (lv_coord_t)y, c);
                lv_img_buf_set_px_alpha(out, (lv_coord_t)x, (lv_coord_t)y, (lv_opa_t)(n ? a / n : 0));
            }
        }
        s_cover = out;
    }
    lv_img_decoder_close(&dsc);
}

/* ---- helpers --------------------------------------------------------------- */

static void fmt_hm(int minutes, char * out, size_t n) { snprintf(out, n, "%d:%02d", minutes / 60, minutes % 60); }

static uint32_t hex_color(const char * s) {
    /* BamBuddy tray colours are RRGGBBAA */
    if (!s || strlen(s) < 6) return THEME_TEXT_MUTED;
    char h[7];
    memcpy(h, s, 6);
    h[6] = 0;
    return (uint32_t)strtoul(h, NULL, 16);
}

/* ---- status view -------------------------------------------------------- */

static void on_close(lv_event_t * e) { (void)e; ui_pop(); }
static void on_settings(lv_event_t * e) { (void)e; s_view = VIEW_SETTINGS; rebuild(); }

/* One temperature cell: caption, light number, target underneath. */
static void temp_cell(lv_obj_t * parent, lv_coord_t x, const char * name, double v, double target) {
    ftui_caption(parent, x, FTUI_GY(2), name);
    char t[24];
    if (v <= -900) snprintf(t, sizeof t, "-");
    else snprintf(t, sizeof t, "%.0f\xc2\xb0", v);
    ftui_number(parent, x - SX(2), FTUI_GY(4), t, 40, target > 0 ? THEME_ACCENT : THEME_TEXT_HI);
    if (target > 0) {
        char g[32];
        snprintf(g, sizeof g, tr("naar %.0f\xc2\xb0", "to %.0f\xc2\xb0"), target);
        ftui_label_at(parent, x, FTUI_GY(12), g, SF(14), THEME_TEXT_DIM);
    }
}

static void build_status(lv_obj_t * body, jv * s) {
    const char * st = jstr(s, "state", "");
    const char * printer = jstr(s, "printer", "");
    const char * file = jstr(s, "file", "");
    long upd = (long)jnum(s, "updated", 0);
    long age = jbool(s, "fresh", 0) && upd > 0 ? (long)(time(NULL) - upd) : -1;
    if (age < 0 && upd > 0) age = (long)(time(NULL) - upd);
    int running = !strcmp(st, "RUNNING") || !strcmp(st, "PAUSE") || !strcmp(st, "PREPARE");
    int finished = !strcmp(st, "FINISH");
    double prog = finished ? 100 : jnum(s, "progress", 0);

    char cap[96];
    snprintf(cap, sizeof cap, "%s  \xe2\x80\xa2  %s", tr("3D-printer", "3D printer"), printer[0] ? printer : "BamBuddy");
    ftui_header(body, cap, jstr(s, "state_label", "?"), on_close);
    ftui_icon_button(body, SX(1024) - FTUI_MARGIN - FTUI_BTN, FTUI_HEAD_Y, LV_SYMBOL_SETTINGS, on_settings, 0);

    /* status line under the settings button: source and age, or the error */
    const char * err = jstr(s, "error", "");
    char foot[160], ago[40];
    ftui_ago(age, ago, sizeof ago);
    if (err[0]) snprintf(foot, sizeof foot, "%s", err);
    else snprintf(foot, sizeof foot, "%s%s", ago, !strcmp(jstr(s, "source", ""), "push") ? "  \xe2\x80\xa2  relay" : "");
    lv_obj_t * fo = ftui_label(body, foot, SF(12), err[0] ? THEME_DANGER : THEME_TEXT_MUTED);
    lv_obj_set_style_text_align(fo, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(fo, SX(300));
    lv_obj_set_pos(fo, SX(1024) - FTUI_MARGIN - FTUI_BTN - FTUI_G(2) - SX(300), FTUI_HEAD_Y + SY(22));

    /* ---- left: ring with the time left ---- */
    lv_coord_t ring = SX(344);
    lv_coord_t rx = FTUI_MARGIN + (SX(360) - ring) / 2, ry = FTUI_TOP + (SY(456) - ring) / 2;
    ftui_ring(body, rx, ry, ring, (int)(prog * 10),
              !strcmp(st, "PAUSE") ? THEME_WARNING : !strcmp(st, "FAILED") ? THEME_DANGER : THEME_ACCENT, SX(6));
    lv_obj_t * mid = ftui_plain(body);
    lv_obj_set_pos(mid, rx, ry);
    lv_obj_set_size(mid, ring, ring);

    char big[24], under[64] = "";
    const char * what;
    int rem = (int)jnum(s, "remaining_min", 0);
    if (running && rem > 0) {
        fmt_hm(rem, big, sizeof big);
        what = tr("resterend", "remaining");
        time_t eta = time(NULL) + (time_t)rem * 60;
        struct tm tm;
        localtime_r(&eta, &tm);
        snprintf(under, sizeof under, tr("%d%%  \xe2\x80\xa2  klaar om %02d:%02d", "%d%%  \xe2\x80\xa2  done at %02d:%02d"),
                 (int)prog, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(big, sizeof big, "%d%%", (int)prog);
        what = finished ? tr("klaar", "finished") : jstr(s, "state_label", "");
        if (jbool(s, "plate_clear", 0)) snprintf(under, sizeof under, "%s", tr("Plaat leegmaken", "Clear the plate"));
    }
    lv_obj_t * bl = ftui_label(mid, big, ftui_light(90), running || finished ? THEME_TEXT_HI : THEME_TEXT_MUTED);
    lv_obj_align(bl, LV_ALIGN_CENTER, 0, -SY(12));
    char wup[48];
    size_t i = 0;
    for (; what[i] && i < sizeof wup - 1; i++) wup[i] = (char)toupper((unsigned char)what[i]);
    wup[i] = 0;
    lv_obj_t * wl = ftui_label(mid, wup, SF(12), THEME_TEXT_MUTED);
    lv_obj_set_style_text_letter_space(wl, 2, 0);
    lv_obj_align_to(wl, bl, LV_ALIGN_OUT_TOP_MID, 0, -SY(4));
    if (under[0]) {
        lv_obj_t * ul = ftui_label(mid, under, SF(14), THEME_TEXT_DIM);
        lv_obj_align_to(ul, bl, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(8));
    }

    /* ---- right column ---- */
    lv_coord_t cx = FTUI_MARGIN + SX(360) + FTUI_G(4);
    lv_coord_t cw = SX(1024) - FTUI_MARGIN - cx;

    /* plate + file */
    lv_obj_t * pc = ftui_card(body, cx, FTUI_TOP, cw, FTUI_GY(25), THEME_CARD_BG);
    lv_coord_t thumb = FTUI_GY(21);
    lv_obj_t * well = ftui_card(pc, FTUI_G(2), FTUI_GY(2), thumb, thumb, THEME_ROW_BG);
    cover_load();
    if (s_cover && jbool(s, "cover", 0)) {
        lv_obj_t * img = lv_img_create(well);
        lv_img_set_src(img, s_cover);
        lv_obj_center(img);
    } else {
        lv_obj_t * ic = ftui_label(well, LV_SYMBOL_IMAGE, SF(28), THEME_TEXT_MUTED);
        lv_obj_center(ic);
    }
    lv_coord_t tx = FTUI_G(2) + thumb + FTUI_G(3), tw = cw - tx - FTUI_G(3);
    ftui_caption(pc, tx, FTUI_GY(3), tr("Bestand", "File"));
    lv_obj_t * fl = ftui_label_at(pc, tx, FTUI_GY(5), file[0] ? file : tr("Geen print", "No print"), SF(22), THEME_TEXT_HI);
    ftui_one_line(fl, tw);
    int layer = (int)jnum(s, "layer", 0), layers = (int)jnum(s, "total_layers", 0);
    if (layers > 0) {
        ftui_caption(pc, tx, FTUI_GY(10), tr("Laag", "Layer"));
        char lt[32];
        snprintf(lt, sizeof lt, "%d / %d", layer, layers);
        ftui_number(pc, tx - SX(2), FTUI_GY(12), lt, 30, THEME_TEXT_HI);
    }
    ftui_bar(pc, tx, FTUI_GY(20), tw, SY(4), (int)(prog * 10), THEME_ACCENT);

    /* temperatures: one surface, four quiet cells */
    lv_coord_t ty = FTUI_TOP + FTUI_GY(25) + FTUI_GAP;
    lv_obj_t * tc = ftui_card(body, cx, ty, cw, FTUI_GY(17), THEME_CARD_BG);
    int dual = jnum(s, "nozzle2", -999) > -900;
    int n = dual ? 4 : 3;
    lv_coord_t cell = (cw - FTUI_G(3)) / n;
    lv_coord_t x = FTUI_G(3);
    temp_cell(tc, x, dual ? tr("Nozzle L", "Nozzle L") : "Nozzle", jnum(s, "nozzle", -999), jnum(s, "nozzle_target", 0));
    x += cell;
    if (dual) {
        temp_cell(tc, x, tr("Nozzle R", "Nozzle R"), jnum(s, "nozzle2", -999), jnum(s, "nozzle2_target", 0));
        x += cell;
    }
    temp_cell(tc, x, tr("Bed", "Bed"), jnum(s, "bed", -999), jnum(s, "bed_target", 0));
    x += cell;
    temp_cell(tc, x, tr("Kamer", "Chamber"), jnum(s, "chamber", -999), jnum(s, "chamber_target", 0));

    /* AMS */
    lv_coord_t ay = ty + FTUI_GY(17) + FTUI_GAP;
    lv_obj_t * ac = ftui_card(body, cx, ay, cw, SY(600) - FTUI_MARGIN - ay, THEME_CARD_BG);
    ftui_caption(ac, FTUI_G(3), FTUI_GY(2), "AMS");
    jv * ams = jget(s, "ams");
    int k = 0;
    lv_coord_t slot = (cw - FTUI_G(3)) / 5;
    for (jv * t = jfirst(ams); t && k < 5; t = t->next, k++) {
        lv_coord_t sx = FTUI_G(3) + k * slot;
        lv_obj_t * dot = ftui_dot(ac, sx, FTUI_GY(5), SX(16), hex_color(jstr(t, "color", "")));
        lv_obj_set_style_outline_width(dot, 1, 0);                 /* white/black spools on a */
        lv_obj_set_style_outline_color(dot, lv_color_hex(THEME_DIVIDER), 0);  /* same-tone card */
        char lab[40];
        int rp = (int)jnum(t, "remain", -1);
        if (rp >= 0) snprintf(lab, sizeof lab, "%s  %d%%", jstr(t, "type", "?"), rp);
        else snprintf(lab, sizeof lab, "%s", jstr(t, "type", "?"));
        lv_obj_t * l = ftui_label_at(ac, sx + SX(24), FTUI_GY(5) - SY(1), lab, SF(14), THEME_TEXT_DIM);
        ftui_one_line(l, slot - SX(28));
    }
    if (k == 0) ftui_label_at(ac, FTUI_G(3), FTUI_GY(5), tr("Geen AMS-gegevens", "No AMS data"), SF(14), THEME_TEXT_MUTED);
}

static void build_nodata(lv_obj_t * body) {
    ftui_header(body, tr("3D-printer", "3D printer"), tr("Nog geen gegevens", "No data yet"), on_close);
    ftui_icon_button(body, SX(1024) - FTUI_MARGIN - FTUI_BTN, FTUI_HEAD_Y, LV_SYMBOL_SETTINGS, on_settings, 0);
    lv_obj_t * c = ftui_card(body, FTUI_MARGIN, FTUI_TOP, SX(1024) - 2 * FTUI_MARGIN, FTUI_GY(22), THEME_CARD_BG);
    ftui_caption(c, FTUI_G(4), FTUI_GY(4), tr("Zo koppel je BamBuddy", "Connecting BamBuddy"));
    lv_obj_t * t = ftui_label_at(c, FTUI_G(4), FTUI_GY(7),
        tr("Vul via het tandwiel het adres van BamBuddy en een API-sleutel in. Kan de Toon BamBuddy niet "
           "bereiken (bijvoorbeeld in een IoT-netwerk), laat dan relay/bambuddy-relay.sh de status naar "
           "de Toon sturen (poort 8766).",
           "Use the gear to enter the BamBuddy address and an API key. If the Toon can't reach BamBuddy "
           "(an IoT network, say), let relay/bambuddy-relay.sh push the status to the Toon (port 8766)."),
        SF(18), THEME_TEXT_DIM);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, SX(1024) - 2 * FTUI_MARGIN - FTUI_G(8));
    lv_obj_set_style_text_line_space(t, SY(6), 0);
}

/* ---- settings view ------------------------------------------------------- */

static void on_settings_back(lv_event_t * e) { (void)e; s_view = VIEW_STATUS; free(s_state_text); s_state_text = NULL; rebuild(); }

static void on_field_done(const char * key, const char * value) {
    if (ftui_conf_set(CONF_PATH, key, value) != 0) fprintf(stderr, "[bambuddy] saving %s failed\n", CONF_PATH);
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
    ftui_header(body, tr("3D-printer", "3D printer"), tr("Instellingen", "Settings"), on_settings_back);
    lv_obj_t * sc = ftui_scroller(body, FTUI_MARGIN, FTUI_TOP, SX(1024) - 2 * FTUI_MARGIN, SY(600) - FTUI_TOP - FTUI_MARGIN);
    lv_coord_t y = 0, w = SX(1024) - 2 * FTUI_MARGIN - SX(8);
    y += ftui_section(sc, y, tr("Opties", "Options"));
    y += ftui_toggle_row(sc, y, w, "notify", tr("Melding", "Notification"),
                         tr("Bericht in de inbox als een print klaar is of mislukt.",
                            "Inbox message when a print finishes or fails."), CONF_PATH) + SY(8);
    y += ftui_toggle_row(sc, y, w, "tile_eta", tr("Eindtijd op de tegel", "Finish time on the tile"),
                         tr("Toon \"klaar om 16:42\" in plaats van de resterende tijd.",
                            "Show \"done at 16:42\" instead of the time left."), CONF_PATH) + SY(8);
    y += ftui_section(sc, y, tr("Verbinding", "Connection"));
    for (int i = 0; i < N_FIELDS; i++) y += ftui_field_row(sc, y, w, &FIELDS[i], CONF_PATH, on_field_edit, i) + SY(8);
    lv_obj_t * h = ftui_label_at(sc, FTUI_G(1), y + SY(8),
        tr("Wijzigingen gelden meteen. Pull: de Toon vraagt BamBuddy zelf. Push: een relay naast BamBuddy stuurt "
           "de status naar poort 8766 van de Toon.",
           "Changes apply right away. Pull: the Toon asks BamBuddy itself. Push: a relay next to BamBuddy sends "
           "the status to port 8766 on the Toon."), SF(14), THEME_TEXT_MUTED);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, w - FTUI_G(2));
}

/* ---- lifecycle ------------------------------------------------------------ */

static void rebuild(void) {
    if (!s_root) return;
    if (s_body) lv_obj_del(s_body);
    s_body = ftui_plain(s_root);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES);
    if (s_view == VIEW_SETTINGS) { build_settings(s_body); return; }
    jv * s = s_state_text ? jparse(s_state_text) : NULL;
    if (s && (jnum(s, "updated", 0) > 0 || jstr(s, "error", "")[0])) build_status(s_body, s);
    else build_nodata(s_body);
    jfree(s);
}

static void on_tick(lv_timer_t * t) {
    (void)t;
    if (ftui_modal_open_() || lv_scr_act() != s_root || s_view != VIEW_STATUS) return;
    char * text = ftui_read(STATE_PATH);
    struct stat st;
    long cm = stat(COVER_PATH, &st) == 0 ? ((long)st.st_mtime ^ (long)st.st_size) : 0;
    if (text && s_state_text && !strcmp(text, s_state_text) && cm == s_cover_mtime) { free(text); return; }
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
    cover_free();
    s_cover_mtime = 0;
}

EXPORT lv_obj_t * screen_create(void) {
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }
    s_view = VIEW_STATUS;
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);
    s_state_text = ftui_read(STATE_PATH);
    rebuild();
    s_timer = lv_timer_create(on_tick, 2000, NULL);
    return s_root;
}
