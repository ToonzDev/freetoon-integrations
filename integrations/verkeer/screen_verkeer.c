/*
 * screen_verkeer.c - detail screen of the "Verkeer" app (verkeer_ui.so).
 *
 * Reads items.tsv written by the daemon (verkeer_daemon.c) and shows:
 *   - files on your roads, then road closures on your roads,
 *   - all files in the Netherlands, longest delay first.
 * "Wegen" opens a keyboard to set the roads to watch (verkeer.conf WEGEN=);
 * the daemon notices the changed file within seconds.
 * Data: NDW open data (CC0).
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"
#include "settings.h"
#include "theme.h"
#include "plugin_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

FT_PLUGIN_DECLARE_ABI(
    FT_SETTINGS_FIELD(home_light_mode));

#define MAX_ROWS 400

typedef struct {
    char   kind;
    int    mine;
    char   road[8];
    double km;
    int    delay_s;
    char   from[48], to[48], cause[96], comment[160];
    long   until;
} vk_item_t;

static lv_obj_t   * s_scr, * s_list, * s_sub, * s_kb_modal, * s_ta;
static lv_timer_t * s_timer;
static time_t       s_mtime;
static char         s_roads[128];

static const char * base_dir(void) {
    const char * d = getenv("VK_DIR");                  /* sim / tests */
    return d && d[0] ? d : "/mnt/data/integrations/verkeer";
}

/* ---- data ----------------------------------------------------------------- */

static vk_item_t * s_items;
static int         s_n, s_nf;
static double      s_km;
static long        s_updated;
static char        s_err[96];

static void field(char ** p, char * out, size_t n) {
    char * t = *p;
    char * e = t ? strchr(t, '\t') : NULL;
    if (e) *e = 0;
    snprintf(out, n, "%s", t ? t : "");
    *p = e ? e + 1 : NULL;
}

static void load_items(void) {
    char path[300], line[700], b[200];
    snprintf(path, sizeof path, "%s/items.tsv", base_dir());
    s_n = s_nf = 0;
    s_km = 0;
    s_updated = 0;
    s_err[0] = s_roads[0] = 0;
    FILE * f = fopen(path, "r");
    if (!f) return;
    if (!s_items) s_items = calloc(MAX_ROWS, sizeof *s_items);
    while (s_items && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char * p = line;
        if (line[0] == '#') {                            /* #  updated  files  km  roads  error */
            field(&p, b, sizeof b);
            field(&p, b, sizeof b); s_updated = atol(b);
            field(&p, b, sizeof b); s_nf = atoi(b);
            field(&p, b, sizeof b); s_km = atof(b);
            field(&p, s_roads, sizeof s_roads);
            field(&p, s_err, sizeof s_err);
            continue;
        }
        if (s_n >= MAX_ROWS) break;
        vk_item_t * it = &s_items[s_n];
        field(&p, b, sizeof b); it->kind = b[0];
        field(&p, b, sizeof b); it->mine = atoi(b);
        field(&p, it->road, sizeof it->road);
        field(&p, b, sizeof b); it->km = atof(b);
        field(&p, b, sizeof b); it->delay_s = atoi(b);
        field(&p, it->from, sizeof it->from);
        field(&p, it->to, sizeof it->to);
        field(&p, it->cause, sizeof it->cause);
        field(&p, it->comment, sizeof it->comment);
        field(&p, b, sizeof b); it->until = atol(b);
        if (it->kind == 'F' || it->kind == 'A') s_n++;
    }
    fclose(f);
}

/* Roads as saved in verkeer.conf (the list in items.tsv lags one round). */
static void conf_roads(char * out, size_t n) {
    char path[300], line[256];
    snprintf(path, sizeof path, "%s/verkeer.conf", base_dir());
    out[0] = 0;
    FILE * f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "WEGEN=", 6)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, n, "%s", line + 6);
        }
    fclose(f);
}

static void save_roads(const char * roads) {
    char path[300], tmp[310], line[256];
    snprintf(path, sizeof path, "%s/verkeer.conf", base_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char keep[1024] = "";
    size_t o = 0;
    FILE * f = fopen(path, "r");
    if (f) {                                             /* keep the other keys */
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "WEGEN=", 6) && o + strlen(line) < sizeof keep) {
                memcpy(keep + o, line, strlen(line) + 1);
                o += strlen(line);
            }
        fclose(f);
    }
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "WEGEN=");
    for (const char * c = roads; *c; c++)                 /* only road-ish characters */
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
            *c == ',' || *c == ' ')
            fputc(*c, f);
    fprintf(f, "\n%s", keep);
    fclose(f);
    rename(tmp, path);
}

/* ---- list ------------------------------------------------------------------- */

static void add_header(const char * txt) {
    lv_obj_t * h = lv_label_create(s_list);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_font(h, SF(18), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(h, 8, 0);
    lv_label_set_text(h, txt);
}

static void add_note(const char * txt) {
    lv_obj_t * h = lv_label_create(s_list);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_font(h, SF(18), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_left(h, 8, 0);
    lv_label_set_text(h, txt);
}

static void add_row(const vk_item_t * it) {
    int is_t1 = LV_VER_RES < 600;
    lv_obj_t * c = lv_obj_create(s_list);
    lv_obj_set_size(c, lv_pct(100), is_t1 ? 56 : SY(64));
    lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_width(c, it->mine && it->kind == 'F' ? 2 : 0, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0xf59e0b), 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    /* road badge in the colours of the signs: A red, N/S yellow */
    int bw = is_t1 ? 52 : SX(64);
    lv_obj_t * bd = lv_label_create(c);
    int is_a = it->road[0] == 'A';
    lv_obj_set_size(bd, bw, is_t1 ? 30 : SY(34));
    lv_obj_set_style_bg_color(bd, lv_color_hex(is_a ? 0xc8102e : 0xf2c500), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bd, 6, 0);
    lv_obj_set_style_text_color(bd, lv_color_hex(is_a ? 0xffffff : 0x111111), 0);
    lv_obj_set_style_text_font(bd, SF(20), 0);
    lv_obj_set_style_text_align(bd, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(bd, is_t1 ? 3 : SY(4), 0);
    lv_label_set_text(bd, it->road);
    lv_obj_align(bd, LV_ALIGN_LEFT_MID, 0, 0);

    int tw = LV_HOR_RES - 40 - bw - 20;
    char l1[160], l2[300];
    if (it->from[0] && it->to[0]) snprintf(l1, sizeof l1, "tussen %s en %s", it->from, it->to);
    else snprintf(l1, sizeof l1, "bij %s", it->to[0] ? it->to : it->from);
    int o = 0;
    l2[0] = 0;
    if (it->km >= 0.1) o += snprintf(l2 + o, sizeof l2 - (size_t)o, "%.1f km", it->km);
    if (it->kind == 'F' && it->delay_s >= 60)
        o += snprintf(l2 + o, sizeof l2 - (size_t)o, "%s+%d min", o ? "  |  " : "", (it->delay_s + 30) / 60);
    if (it->kind == 'A') {
        if (it->until > 0) {
            char tb[40];
            time_t u = (time_t)it->until;
            strftime(tb, sizeof tb, "tot %d-%m %H:%M", localtime(&u));
            o += snprintf(l2 + o, sizeof l2 - (size_t)o, "%s%s", o ? "  |  " : "", tb);
        }
        const char * what = it->comment[0] ? it->comment : (it->cause[0] ? it->cause : "afgesloten");
        o += snprintf(l2 + o, sizeof l2 - (size_t)o, "%s%s", o ? "  |  " : "", what);
    } else if (it->cause[0])
        o += snprintf(l2 + o, sizeof l2 - (size_t)o, "%s%s", o ? "  |  " : "", it->cause);

    lv_obj_t * a = lv_label_create(c);
    lv_obj_set_style_text_font(a, SF(18), 0);
    lv_obj_set_style_text_color(a, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_size(a, tw, lv_font_get_line_height(SF(18)));
    lv_label_set_text(a, l1);
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, bw + 12, 0);

    lv_obj_t * b = lv_label_create(c);
    lv_obj_set_style_text_font(b, SF(16), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(it->kind == 'F' && it->delay_s >= 600 ? 0xf59e0b : THEME_TEXT_DIM), 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    lv_obj_set_size(b, tw, lv_font_get_line_height(SF(16)));
    lv_label_set_text(b, l2);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, bw + 12, 0);
}

static void rebuild(void) {
    if (!s_list) return;
    load_items();
    lv_obj_clean(s_list);
    char sub[200], tb[16] = "";
    if (s_updated) {
        time_t u = (time_t)s_updated;
        strftime(tb, sizeof tb, "%H:%M", localtime(&u));
    }
    if (!s_updated) snprintf(sub, sizeof sub, "Nog geen gegevens: de app haalt ze zo op.");
    else if (s_err[0]) snprintf(sub, sizeof sub, "%s (%s)", s_err, tb);
    else snprintf(sub, sizeof sub, "Nederland: %d file%s, %.0f km  |  %s  |  bron: NDW", s_nf, s_nf == 1 ? "" : "s", s_km, tb);
    lv_label_set_text(s_sub, sub);

    char roads[128];
    conf_roads(roads, sizeof roads);
    char h[200];
    if (roads[0]) {
        int mf = 0, ma = 0;
        for (int i = 0; i < s_n; i++) {
            if (!s_items[i].mine) continue;
            if (s_items[i].kind == 'F') mf++; else ma++;
        }
        snprintf(h, sizeof h, "Files op je wegen (%s)", roads);
        add_header(h);
        if (!mf) add_note(s_updated ? "Geen files." : "...");
        for (int i = 0; i < s_n; i++)
            if (s_items[i].kind == 'F' && s_items[i].mine) add_row(&s_items[i]);
        add_header("Afsluitingen op je wegen");
        if (!ma) add_note(s_updated ? "Geen afsluitingen." : "...");
        for (int i = 0; i < s_n; i++)
            if (s_items[i].kind == 'A' && s_items[i].mine) add_row(&s_items[i]);
    } else {
        add_note("Tip: kies met 'Wegen' de wegen die je rijdt (bijv. A2, A12); dan staan hun files en afsluitingen bovenaan en op de tegel.");
    }
    add_header("Alle files in Nederland");
    int any = 0;
    for (int i = 0; i < s_n; i++)
        if (s_items[i].kind == 'F') { add_row(&s_items[i]); any = 1; }
    if (!any) add_note(s_updated ? "Geen files." : "...");
}

static void on_timer(lv_timer_t * t) {
    (void)t;
    char path[300];
    struct stat st;
    snprintf(path, sizeof path, "%s/items.tsv", base_dir());
    if (stat(path, &st) == 0 && st.st_mtime != s_mtime) {
        s_mtime = st.st_mtime;
        rebuild();
    }
}

/* ---- roads keyboard ---------------------------------------------------------- */

static void kb_close(void) {
    if (s_kb_modal) { lv_obj_del(s_kb_modal); s_kb_modal = NULL; s_ta = NULL; }
}
static void on_kb(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) {
        save_roads(lv_textarea_get_text(s_ta));
        kb_close();
        rebuild();
    } else if (c == LV_EVENT_CANCEL) {
        kb_close();
    }
}
static void on_roads(lv_event_t * e) {
    (void)e;
    if (s_kb_modal) return;
    s_kb_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_kb_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_kb_modal, 0, 0);
    lv_obj_set_style_bg_color(s_kb_modal, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(s_kb_modal, 0, 0);
    lv_obj_set_style_radius(s_kb_modal, 0, 0);
    lv_obj_set_style_pad_all(s_kb_modal, 0, 0);
    lv_obj_clear_flag(s_kb_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * l = lv_label_create(s_kb_modal);
    lv_obj_set_style_text_font(l, SF(20), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(l, "Jouw wegen, met komma's (bijv. A2, A12, N201). Leeg = alleen landelijk.");
    lv_obj_set_width(l, LV_HOR_RES - 40);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, 20, SY(16));
    s_ta = lv_textarea_create(s_kb_modal);
    lv_textarea_set_one_line(s_ta, true);
    lv_obj_set_width(s_ta, LV_HOR_RES - 40);
    lv_obj_set_pos(s_ta, 20, SY(70));
    char roads[128];
    conf_roads(roads, sizeof roads);
    lv_textarea_set_text(s_ta, roads);
    lv_obj_t * kb = lv_keyboard_create(s_kb_modal);
    lv_obj_set_size(kb, LV_PCT(100), LV_VER_RES < 600 ? SY(260) : SY(300));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_obj_add_event_cb(kb, on_kb, LV_EVENT_ALL, NULL);
}

/* ---- screen -------------------------------------------------------------------- */

static void on_close(lv_event_t * e) { (void)e; ui_pop(); }

static void on_deleted(lv_event_t * e) {
    (void)e;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_scr = s_list = s_sub = s_kb_modal = s_ta = NULL;
    s_mtime = 0;
}

static lv_obj_t * top_btn(const char * txt, int w, int right) {
    int h = LV_VER_RES < 600 ? 38 : 44;
    lv_obj_t * b = lv_btn_create(s_scr);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -right, SY(8));
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t * screen_create(void) {
    int is_t1 = LV_VER_RES < 600;
    s_scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(s_scr, on_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(s_scr);
    lv_obj_set_style_text_font(t, SF(28), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(t, "Verkeer");
    lv_obj_set_pos(t, SX(16), SY(8));

    s_sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_sub, SF(16), 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_sub, LV_HOR_RES - SX(32), lv_font_get_line_height(SF(16)));
    lv_label_set_text(s_sub, "");
    lv_obj_set_pos(s_sub, SX(16), is_t1 ? 46 : SY(52));

    int xw = is_t1 ? 52 : 60;
    lv_obj_t * x = top_btn(LV_SYMBOL_CLOSE, xw, SX(12));
    lv_obj_add_event_cb(x, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * w = top_btn("Wegen", is_t1 ? 100 : SX(120), SX(12) + xw + SX(10));
    lv_obj_add_event_cb(w, on_roads, LV_EVENT_CLICKED, NULL);

    int ly = is_t1 ? 70 : SY(80);
    s_list = lv_obj_create(s_scr);
    lv_obj_set_size(s_list, LV_HOR_RES - 20, LV_VER_RES - ly - 6);
    lv_obj_set_pos(s_list, 10, ly);
    lv_obj_set_style_bg_opa(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 2, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);

    rebuild();
    struct stat st;
    char path[300];
    snprintf(path, sizeof path, "%s/items.tsv", base_dir());
    s_mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
    s_timer = lv_timer_create(on_timer, 2000, NULL);
    return s_scr;
}
