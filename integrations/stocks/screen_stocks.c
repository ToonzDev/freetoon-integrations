/*
 * screen_stocks.c — FreeToon Stock Exchange & AEX Interactive Screen
 *
 * Full-screen stock quote and interactive chart viewer.
 * Features:
 *   - Live quote, day change percentage and points, day range, 52-week high/low.
 *   - Interactive multi-timeframe chart: 1D, 5D, 1M, 6M, 1Y.
 *   - Quick-switch preset chips for popular Dutch stocks (AEX, ASML, Shell, ING, etc.).
 *   - Comprehensive Stock Picker modal with Dutch & Global stocks/indices and
 *     on-screen keyboard for any custom ticker symbol.
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
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <math.h>

#define BASE_DIR        "/mnt/data/integrations/stocks"
#define CONF_PATH       BASE_DIR "/stocks.conf"
#define HIST_DIR        BASE_DIR "/hist"
#define STATE_PATH      BASE_DIR "/state.json"
#define TRIG_PATH       BASE_DIR "/reload.trig"

#define MAX_CHART_PTS   256
#define MAX_FAVS        8

typedef struct {
    char ticker[32];
    char name[48];
} stock_preset_t;

static const stock_preset_t s_all_presets[] = {
    /* Nederlandse Indices */
    { "^AEX",        "AEX-Index" },
    { "^AMX",        "AMX Midcap" },
    { "^ASCX",       "AScX Smallcap" },
    /* Nederlandse AEX Aandelen */
    { "ASML.AS",     "ASML" },
    { "SHELL.AS",    "Shell" },
    { "INGA.AS",     "ING Groep" },
    { "AD.AS",       "Ahold Delhaize" },
    { "HEIA.AS",     "Heineken" },
    { "PHIA.AS",     "Philips" },
    { "UNA.AS",      "Unilever" },
    { "KPN.AS",      "KPN" },
    { "NN.AS",       "NN Group" },
    { "BESI.AS",     "Besi" },
    { "ASM.AS",      "ASM International" },
    { "WKL.AS",      "Wolters Kluwer" },
    { "DSFIR.AS",    "DSM-Firmenich" },
    { "PRX.AS",      "Prosus" },
    { "AGN.AS",      "Aegon" },
    { "ABN.AS",      "ABN AMRO" },
    { "TKWY.AS",     "Just Eat Takeaway" },
    { "ARCAD.AS",    "Arcadis" },
    { "FLOW.AS",     "Flow Traders" },
    /* Wereldwijd */
    { "^GSPC",       "S&P 500" },
    { "^IXIC",       "Nasdaq 100" },
    { "^DJI",        "Dow Jones" },
    { "^GDAXI",      "DAX 40" },
    { "AAPL",        "Apple" },
    { "MSFT",        "Microsoft" },
    { "NVDA",        "Nvidia" },
    { "TSLA",        "Tesla" },
    { "GOOGL",       "Alphabet" },
    { "AMZN",        "Amazon" },
};
#define NUM_ALL_PRESETS (sizeof(s_all_presets) / sizeof(s_all_presets[0]))

static stock_preset_t s_favs[MAX_FAVS];
static int s_fav_count = 0;

static lv_obj_t * s_modal = NULL;
static lv_obj_t * s_lbl_title = NULL;
static lv_obj_t * s_lbl_price = NULL;
static lv_obj_t * s_badge_chg = NULL;
static lv_obj_t * s_lbl_chg = NULL;
static lv_obj_t * s_lbl_info = NULL;

static lv_obj_t * s_fav_btns[MAX_FAVS];
static lv_obj_t * s_btn_more = NULL;

static lv_obj_t * s_tf_btns[5];
static const char * const s_tf_keys[5] = { "1d", "5d", "1mo", "6mo", "1y" };
static const char * const s_tf_labels[5] = { "1D", "5D", "1M", "6M", "1J" };
static int s_cur_tf = 0; /* default: 1D */

static lv_obj_t * s_chart = NULL;
static lv_chart_series_t * s_series = NULL;
static lv_obj_t * s_lbl_hi = NULL;
static lv_obj_t * s_lbl_lo = NULL;

static char s_cur_ticker[32] = "^AEX";
static char s_cur_name[64] = "AEX-Index";

/* Modal picker elements */
static lv_obj_t * s_picker_modal = NULL;
static lv_obj_t * s_picker_ta = NULL;
static lv_obj_t * s_picker_kb = NULL;

static void load_state_and_refresh(void);
static void open_picker_modal(void);
static void close_picker_modal(void);

static void make_safe_ticker(const char * src, char * dst, size_t maxlen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < maxlen; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == '^') {
            dst[j++] = 'X';
        } else if (c == '.') {
            dst[j++] = '_';
        }
    }
    dst[j] = 0;
}

static void read_config(void) {
    s_fav_count = 0;
    FILE * f = fopen(CONF_PATH, "r");
    if (!f) {
        snprintf(s_cur_ticker, sizeof(s_cur_ticker), "^AEX");
        snprintf(s_cur_name, sizeof(s_cur_name), "AEX-Index");
        snprintf(s_favs[0].ticker, sizeof(s_favs[0].ticker), "^AEX");
        snprintf(s_favs[0].name, sizeof(s_favs[0].name), "AEX");
        snprintf(s_favs[1].ticker, sizeof(s_favs[1].ticker), "ASML.AS");
        snprintf(s_favs[1].name, sizeof(s_favs[1].name), "ASML");
        snprintf(s_favs[2].ticker, sizeof(s_favs[2].ticker), "SHELL.AS");
        snprintf(s_favs[2].name, sizeof(s_favs[2].name), "Shell");
        snprintf(s_favs[3].ticker, sizeof(s_favs[3].ticker), "INGA.AS");
        snprintf(s_favs[3].name, sizeof(s_favs[3].name), "ING");
        snprintf(s_favs[4].ticker, sizeof(s_favs[4].ticker), "AD.AS");
        snprintf(s_favs[4].name, sizeof(s_favs[4].name), "Ahold");
        snprintf(s_favs[5].ticker, sizeof(s_favs[5].ticker), "HEIA.AS");
        snprintf(s_favs[5].name, sizeof(s_favs[5].name), "Heineken");
        s_fav_count = 6;
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char * nl = strchr(line, '\r'); if (nl) *nl = 0;
        nl = strchr(line, '\n'); if (nl) *nl = 0;

        if (strncmp(line, "TICKER=", 7) == 0) {
            snprintf(s_cur_ticker, sizeof(s_cur_ticker), "%s", line + 7);
        } else if (strncmp(line, "NAME=", 5) == 0) {
            snprintf(s_cur_name, sizeof(s_cur_name), "%s", line + 5);
        } else if (strncmp(line, "FAV=", 4) == 0 && s_fav_count < MAX_FAVS) {
            char * v = line + 4;
            char * comma = strchr(v, ',');
            if (comma) {
                *comma = 0;
                snprintf(s_favs[s_fav_count].ticker, sizeof(s_favs[s_fav_count].ticker), "%s", v);
                snprintf(s_favs[s_fav_count].name, sizeof(s_favs[s_fav_count].name), "%s", comma + 1);
            } else {
                snprintf(s_favs[s_fav_count].ticker, sizeof(s_favs[s_fav_count].ticker), "%s", v);
                snprintf(s_favs[s_fav_count].name, sizeof(s_favs[s_fav_count].name), "%s", v);
            }
            s_fav_count++;
        }
    }
    fclose(f);

    if (s_fav_count == 0) {
        snprintf(s_favs[0].ticker, sizeof(s_favs[0].ticker), "^AEX");
        snprintf(s_favs[0].name, sizeof(s_favs[0].name), "AEX");
        snprintf(s_favs[1].ticker, sizeof(s_favs[1].ticker), "ASML.AS");
        snprintf(s_favs[1].name, sizeof(s_favs[1].name), "ASML");
        snprintf(s_favs[2].ticker, sizeof(s_favs[2].ticker), "SHELL.AS");
        snprintf(s_favs[2].name, sizeof(s_favs[2].name), "Shell");
        snprintf(s_favs[3].ticker, sizeof(s_favs[3].ticker), "INGA.AS");
        snprintf(s_favs[3].name, sizeof(s_favs[3].name), "ING");
        snprintf(s_favs[4].ticker, sizeof(s_favs[4].ticker), "AD.AS");
        snprintf(s_favs[4].name, sizeof(s_favs[4].name), "Ahold");
        snprintf(s_favs[5].ticker, sizeof(s_favs[5].ticker), "HEIA.AS");
        snprintf(s_favs[5].name, sizeof(s_favs[5].name), "Heineken");
        s_fav_count = 6;
    }
}

static void save_selected_ticker(const char * ticker, const char * name) {
    snprintf(s_cur_ticker, sizeof(s_cur_ticker), "%s", ticker);
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", name);

    /* Update stocks.conf */
    mkdir(BASE_DIR, 0755);
    FILE * f = fopen(CONF_PATH, "w");
    if (f) {
        fprintf(f, "TICKER=%s\n", s_cur_ticker);
        fprintf(f, "NAME=%s\n", s_cur_name);
        fprintf(f, "TIMEFRAME=%s\n", s_tf_keys[s_cur_tf]);
        for (int i = 0; i < s_fav_count; i++) {
            fprintf(f, "FAV=%s,%s\n", s_favs[i].ticker, s_favs[i].name);
        }
        fclose(f);
    }

    /* Signal daemon for immediate refresh */
    FILE * trg = fopen(TRIG_PATH, "w");
    if (trg) {
        fprintf(trg, "1\n");
        fclose(trg);
    }
}

static void on_close_click(lv_event_t * e) {
    (void)e;
    ui_pop();
}

static void on_refresh_click(lv_event_t * e) {
    (void)e;
    FILE * trg = fopen(TRIG_PATH, "w");
    if (trg) {
        fprintf(trg, "1\n");
        fclose(trg);
    }
    load_state_and_refresh();
}

static void on_fav_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_fav_count) {
        save_selected_ticker(s_favs[idx].ticker, s_favs[idx].name);
        load_state_and_refresh();
    }
}

static void on_tf_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 5) {
        s_cur_tf = idx;
        load_state_and_refresh();
    }
}

static void on_more_click(lv_event_t * e) {
    (void)e;
    open_picker_modal();
}

static void on_modal_deleted(lv_event_t * e) {
    (void)e;
    s_modal = NULL;
}

/* Parse json string field */
static int extract_json_str(const char * json, const char * key, char * out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    const char * p = strstr(json, pat);
    if (!p) {
        snprintf(pat, sizeof(pat), "\"%s\":\"", key);
        p = strstr(json, pat);
        if (!p) return 0;
        p += strlen(pat);
    } else {
        p += strlen(pat);
    }

    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return (i > 0);
}

/* Parse json number field */
static int extract_json_num(const char * json, const char * key, double * out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char * p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    char * end = NULL;
    double d = strtod(p, &end);
    if (end != p) {
        *out = d;
        return 1;
    }
    return 0;
}

static void load_state_and_refresh(void) {
    if (!s_modal) return;

    /* Highlight active fav button */
    for (int i = 0; i < s_fav_count && i < MAX_FAVS; i++) {
        if (!s_fav_btns[i]) continue;
        int is_active = (strcmp(s_favs[i].ticker, s_cur_ticker) == 0);
        lv_obj_set_style_bg_color(s_fav_btns[i], lv_color_hex(is_active ? THEME_BTN_ACCENT : THEME_BTN_BG), 0);
    }

    /* Highlight active timeframe button */
    for (int i = 0; i < 5; i++) {
        if (!s_tf_btns[i]) continue;
        int is_active = (i == s_cur_tf);
        lv_obj_set_style_bg_color(s_tf_btns[i], lv_color_hex(is_active ? THEME_BTN_ACCENT : THEME_BTN_BG), 0);
    }

    /* Read state.json */
    char buf[4096];
    FILE * f = fopen(STATE_PATH, "r");
    char val[32] = "", sub[32] = "", name[64] = "", ticker[32] = "";
    double change = 0.0, day_hi = 0.0, day_lo = 0.0, hi_52w = 0.0, lo_52w = 0.0;

    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);

        extract_json_str(buf, "ticker", ticker, sizeof(ticker));
        extract_json_str(buf, "name", name, sizeof(name));
        extract_json_str(buf, "value", val, sizeof(val));
        extract_json_str(buf, "subtitle", sub, sizeof(sub));
        extract_json_num(buf, "change", &change);
        extract_json_num(buf, "day_high", &day_hi);
        extract_json_num(buf, "day_low", &day_lo);
        extract_json_num(buf, "high_52w", &hi_52w);
        extract_json_num(buf, "low_52w", &lo_52w);
    }

    /* If state.json is for current ticker, display it; otherwise show loading */
    if (ticker[0] && strcmp(ticker, s_cur_ticker) == 0) {
        char title_buf[96];
        snprintf(title_buf, sizeof(title_buf), "%s (%s)", name[0] ? name : s_cur_name, s_cur_ticker);
        lv_label_set_text(s_lbl_title, title_buf);
        lv_label_set_text(s_lbl_price, val[0] ? val : "...");
        lv_label_set_text(s_lbl_chg, sub[0] ? sub : "");

        uint32_t badge_col = (change >= 0) ? 0x16a34a : 0xdc2626;
        lv_obj_set_style_bg_color(s_badge_chg, lv_color_hex(badge_col), 0);

        char info_buf[128];
        if (day_hi > 0.01 && hi_52w > 0.01) {
            snprintf(info_buf, sizeof(info_buf), "Dag: %.2f - %.2f   |   52w: %.2f - %.2f",
                     day_lo, day_hi, lo_52w, hi_52w);
        } else {
            snprintf(info_buf, sizeof(info_buf), "Koershistorie %s", s_tf_labels[s_cur_tf]);
        }
        lv_label_set_text(s_lbl_info, info_buf);
    } else {
        char title_buf[96];
        snprintf(title_buf, sizeof(title_buf), "%s (%s)", s_cur_name, s_cur_ticker);
        lv_label_set_text(s_lbl_title, title_buf);
        lv_label_set_text(s_lbl_price, "Ophalen...");
        lv_label_set_text(s_lbl_chg, "...");
        lv_obj_set_style_bg_color(s_badge_chg, lv_color_hex(0x4b5563), 0);
        lv_label_set_text(s_lbl_info, "Live koers wordt bijgewerkt...");
    }

    /* Load TSV chart points */
    char safe_tk[64];
    make_safe_ticker(s_cur_ticker, safe_tk, sizeof(safe_tk));

    char tsv_path[256];
    snprintf(tsv_path, sizeof(tsv_path), "%s/%s_%s.tsv", HIST_DIR, safe_tk, s_tf_keys[s_cur_tf]);

    FILE * tf = fopen(tsv_path, "r");
    double prices[MAX_CHART_PTS];
    int pt_count = 0;
    double min_p = 1e9, max_p = -1e9;

    if (tf) {
        char tline[128];
        while (fgets(tline, sizeof(tline), tf) && pt_count < MAX_CHART_PTS) {
            char * tab = strchr(tline, '\t');
            if (tab) {
                double p = strtod(tab + 1, NULL);
                if (p > 0.0001) {
                    prices[pt_count++] = p;
                    if (p < min_p) min_p = p;
                    if (p > max_p) max_p = p;
                }
            }
        }
        fclose(tf);
    }

    lv_chart_set_point_count(s_chart, (pt_count > 10) ? pt_count : 10);

    if (pt_count >= 2) {
        double span = max_p - min_p;
        if (span < 0.01) span = 1.0;
        double pad = span * 0.08;
        double chart_min = min_p - pad;
        double chart_max = max_p + pad;

        /* Range in integer coordinates 0..1000 */
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);

        for (int i = 0; i < pt_count; i++) {
            int norm = (int)(((prices[i] - chart_min) / (chart_max - chart_min)) * 1000.0);
            if (norm < 0) norm = 0;
            if (norm > 1000) norm = 1000;
            lv_chart_set_next_value(s_chart, s_series, (lv_coord_t)norm);
        }

        /* Color green if end >= start, else red */
        uint32_t line_color = (prices[pt_count - 1] >= prices[0]) ? 0x16a34a : 0xdc2626;
        lv_chart_set_series_color(s_chart, s_series, lv_color_hex(line_color));

        char hi_buf[32], lo_buf[32];
        snprintf(hi_buf, sizeof(hi_buf), "Hoog: %.2f", max_p);
        snprintf(lo_buf, sizeof(lo_buf), "Laag: %.2f", min_p);
        lv_label_set_text(s_lbl_hi, hi_buf);
        lv_label_set_text(s_lbl_lo, lo_buf);
    } else {
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        for (int i = 0; i < 10; i++) {
            lv_chart_set_next_value(s_chart, s_series, 50);
        }
        lv_label_set_text(s_lbl_hi, "");
        lv_label_set_text(s_lbl_lo, "");
    }
    lv_chart_refresh(s_chart);
}

/* ===================================================================== */
/* Stock Picker Modal (Categorized grid & on-screen custom ticker input)  */
/* ===================================================================== */

static void on_picker_item_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < (int)NUM_ALL_PRESETS) {
        save_selected_ticker(s_all_presets[idx].ticker, s_all_presets[idx].name);
        close_picker_modal();
        load_state_and_refresh();
    }
}

static void on_picker_custom_select(void) {
    if (!s_picker_ta) return;
    const char * txt = lv_textarea_get_text(s_picker_ta);
    if (!txt || !txt[0]) return;

    char cleaned[32] = "";
    size_t j = 0;
    for (size_t i = 0; txt[i] && j + 1 < sizeof(cleaned); i++) {
        char c = txt[i];
        if (isalnum((unsigned char)c) || c == '.' || c == '^' || c == '-') {
            cleaned[j++] = toupper((unsigned char)c);
        }
    }
    cleaned[j] = 0;

    if (cleaned[0]) {
        save_selected_ticker(cleaned, cleaned);
        close_picker_modal();
        load_state_and_refresh();
    }
}

static void on_picker_custom_click(lv_event_t * e) {
    (void)e;
    on_picker_custom_select();
}

static void on_picker_kb_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_picker_custom_select();
    } else if (code == LV_EVENT_CANCEL) {
        close_picker_modal();
    }
}

static void close_picker_modal(void) {
    if (s_picker_modal) {
        lv_obj_del(s_picker_modal);
        s_picker_modal = NULL;
        s_picker_ta = NULL;
        s_picker_kb = NULL;
    }
}

static void on_picker_close_click(lv_event_t * e) {
    (void)e;
    close_picker_modal();
}

static void open_picker_modal(void) {
    if (s_picker_modal) close_picker_modal();

    s_picker_modal = lv_obj_create(s_modal);
    lv_obj_set_size(s_picker_modal, LV_PCT(94), LV_PCT(92));
    lv_obj_center(s_picker_modal);
    lv_obj_set_style_bg_color(s_picker_modal, lv_color_hex(0x18181b), 0);
    lv_obj_set_style_border_color(s_picker_modal, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_picker_modal, 2, 0);
    lv_obj_set_style_radius(s_picker_modal, 14, 0);
    lv_obj_set_style_pad_all(s_picker_modal, SX(12), 0);

    /* Header */
    lv_obj_t * lbl_head = lv_label_create(s_picker_modal);
    lv_label_set_text(lbl_head, "Kies Aandeel of Index");
    lv_obj_set_style_text_font(lbl_head, SF(20), 0);
    lv_obj_set_style_text_color(lbl_head, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_align(lbl_head, LV_ALIGN_TOP_LEFT, SX(6), SY(4));

    /* Close Button */
    lv_obj_t * btn_cls = lv_btn_create(s_picker_modal);
    lv_obj_set_size(btn_cls, SX(48), SY(36));
    lv_obj_align(btn_cls, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_cls, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_cls, 8, 0);
    lv_obj_add_event_cb(btn_cls, on_picker_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l_cls = lv_label_create(btn_cls);
    lv_label_set_text(l_cls, LV_SYMBOL_CLOSE);
    lv_obj_center(l_cls);

    /* Custom Ticker Row: Textarea + Button */
    s_picker_ta = lv_textarea_create(s_picker_modal);
    lv_obj_set_size(s_picker_ta, SX(280), SY(40));
    lv_obj_set_pos(s_picker_ta, SX(6), SY(42));
    lv_textarea_set_one_line(s_picker_ta, true);
    lv_textarea_set_placeholder_text(s_picker_ta, "Symbool (bijv. NVDA, AAPL)...");
    lv_obj_set_style_bg_color(s_picker_ta, lv_color_hex(0x27272a), 0);
    lv_obj_set_style_text_color(s_picker_ta, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_border_color(s_picker_ta, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_radius(s_picker_ta, 8, 0);

    lv_obj_t * btn_custom = lv_btn_create(s_picker_modal);
    lv_obj_set_size(btn_custom, SX(110), SY(40));
    lv_obj_set_pos(btn_custom, SX(294), SY(42));
    lv_obj_set_style_bg_color(btn_custom, lv_color_hex(THEME_BTN_ACCENT), 0);
    lv_obj_set_style_radius(btn_custom, 8, 0);
    lv_obj_add_event_cb(btn_custom, on_picker_custom_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lc = lv_label_create(btn_custom);
    lv_label_set_text(lc, "Toepassen");
    lv_obj_center(lc);

    /* Grid of Preset Buttons in a scrollable container */
    int cont_y = SY(88);
    int cont_h = (LV_VER_RES < 600) ? SY(190) : SY(240);

    lv_obj_t * cont = lv_obj_create(s_picker_modal);
    lv_obj_set_size(cont, LV_PCT(100), cont_h);
    lv_obj_set_pos(cont, 0, cont_y);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x18181b), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, SX(4), 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < NUM_ALL_PRESETS; i++) {
        lv_obj_t * b = lv_btn_create(cont);
        lv_obj_set_size(b, SX(135), SY(38));
        lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, SX(2), 0);
        lv_obj_add_event_cb(b, on_picker_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, s_all_presets[i].name);
        lv_obj_set_style_text_font(l, SF(14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(l);
    }

    /* On-Screen Virtual Keyboard */
    s_picker_kb = lv_keyboard_create(s_picker_modal);
    int kb_h = (LV_VER_RES < 600) ? SY(140) : SY(170);
    lv_obj_set_size(s_picker_kb, LV_PCT(100), kb_h);
    lv_obj_align(s_picker_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_picker_kb, s_picker_ta);
    lv_obj_add_event_cb(s_picker_kb, on_picker_kb_event, LV_EVENT_ALL, NULL);
}

/* ===================================================================== */
/* Main Detail Screen Creation                                            */
/* ===================================================================== */

lv_obj_t * screen_create(void) {
    if (s_modal) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }

    read_config();

    s_modal = lv_obj_create(NULL);
    lv_obj_add_event_cb(s_modal, on_modal_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(s_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Top Right Buttons: Close & Refresh */
    int x_w = (LV_VER_RES < 600) ? 56 : 64;
    int x_h = (LV_VER_RES < 600) ? 42 : 46;

    lv_obj_t * b_close = lv_btn_create(s_modal);
    lv_obj_set_size(b_close, x_w, x_h);
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(b_close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_close, 10, 0);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_close = lv_label_create(b_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_close, SF(20), 0);
    lv_obj_center(lbl_close);

    lv_obj_t * b_ref = lv_btn_create(s_modal);
    lv_obj_set_size(b_ref, x_w, x_h);
    lv_obj_align(b_ref, LV_ALIGN_TOP_RIGHT, SX(-12) - x_w - SX(8), SY(8));
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_ref, 10, 0);
    lv_obj_add_event_cb(b_ref, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ref = lv_label_create(b_ref);
    lv_label_set_text(lbl_ref, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_ref, SF(18), 0);
    lv_obj_center(lbl_ref);

    /* Stock Name / Symbol */
    s_lbl_title = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_title, s_cur_name);
    lv_obj_set_style_text_font(s_lbl_title, SF(22), 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_pos(s_lbl_title, SX(16), SY(10));

    /* Current Price Label */
    s_lbl_price = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_price, "...");
    const lv_font_t * pf = (LV_VER_RES < 600) ? SF(28) : SF(36);
    lv_obj_set_style_text_font(s_lbl_price, pf, 0);
    lv_obj_set_style_text_color(s_lbl_price, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_pos(s_lbl_price, SX(16), SY(36));

    /* Change Badge Pill */
    s_badge_chg = lv_obj_create(s_modal);
    lv_obj_set_size(s_badge_chg, SX(160), SY(32));
    lv_obj_set_pos(s_badge_chg, SX(240), SY(40));
    lv_obj_set_style_bg_color(s_badge_chg, lv_color_hex(0x16a34a), 0);
    lv_obj_set_style_radius(s_badge_chg, 16, 0);
    lv_obj_set_style_border_width(s_badge_chg, 0, 0);
    lv_obj_set_style_pad_all(s_badge_chg, 0, 0);
    lv_obj_clear_flag(s_badge_chg, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_chg = lv_label_create(s_badge_chg);
    lv_label_set_text(s_lbl_chg, "...");
    lv_obj_set_style_text_font(s_lbl_chg, SF(16), 0);
    lv_obj_set_style_text_color(s_lbl_chg, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_lbl_chg);

    /* Secondary Info / Day Range */
    s_lbl_info = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_info, "...");
    lv_obj_set_style_text_font(s_lbl_info, SF(14), 0);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_pos(s_lbl_info, SX(16), SY(78));

    /* Quick Preset Stock Chips Row */
    int chip_y = SY(104);
    int chip_w = SX(84);
    int chip_h = SY(34);
    int chip_gap = SX(6);

    for (int i = 0; i < s_fav_count && i < MAX_FAVS; i++) {
        s_fav_btns[i] = lv_btn_create(s_modal);
        lv_obj_set_size(s_fav_btns[i], chip_w, chip_h);
        lv_obj_set_pos(s_fav_btns[i], SX(16) + i * (chip_w + chip_gap), chip_y);
        lv_obj_set_style_bg_color(s_fav_btns[i], lv_color_hex(THEME_BTN_BG), 0);
        lv_obj_set_style_radius(s_fav_btns[i], 8, 0);
        lv_obj_set_style_pad_all(s_fav_btns[i], 0, 0);
        lv_obj_add_event_cb(s_fav_btns[i], on_fav_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(s_fav_btns[i]);
        lv_label_set_text(l, s_favs[i].name);
        lv_obj_set_style_text_font(l, SF(14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(l);
    }

    /* "+ Kiezen" button */
    s_btn_more = lv_btn_create(s_modal);
    lv_obj_set_size(s_btn_more, SX(92), chip_h);
    lv_obj_set_pos(s_btn_more, SX(16) + s_fav_count * (chip_w + chip_gap), chip_y);
    lv_obj_set_style_bg_color(s_btn_more, lv_color_hex(0x27272a), 0);
    lv_obj_set_style_border_color(s_btn_more, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_more, 1, 0);
    lv_obj_set_style_radius(s_btn_more, 8, 0);
    lv_obj_set_style_pad_all(s_btn_more, 0, 0);
    lv_obj_add_event_cb(s_btn_more, on_more_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lm = lv_label_create(s_btn_more);
    lv_label_set_text(lm, "+ Kiezen");
    lv_obj_set_style_text_font(lm, SF(14), 0);
    lv_obj_set_style_text_color(lm, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(lm);

    /* Timeframe Selector Chips Row (1D, 5D, 1M, 6M, 1J) */
    int tf_chip_w = SX(56);
    int tf_start_x = LV_HOR_RES - SX(16) - 5 * (tf_chip_w + chip_gap);
    for (int i = 0; i < 5; i++) {
        s_tf_btns[i] = lv_btn_create(s_modal);
        lv_obj_set_size(s_tf_btns[i], tf_chip_w, chip_h);
        lv_obj_set_pos(s_tf_btns[i], tf_start_x + i * (tf_chip_w + chip_gap), chip_y);
        lv_obj_set_style_bg_color(s_tf_btns[i], lv_color_hex(THEME_BTN_BG), 0);
        lv_obj_set_style_radius(s_tf_btns[i], 8, 0);
        lv_obj_set_style_pad_all(s_tf_btns[i], 0, 0);
        lv_obj_add_event_cb(s_tf_btns[i], on_tf_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(s_tf_btns[i]);
        lv_label_set_text(l, s_tf_labels[i]);
        lv_obj_set_style_text_font(l, SF(14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(l);
    }

    /* Chart Container & lv_chart */
    int chart_y = chip_y + chip_h + SY(10);
    int chart_h = LV_VER_RES - chart_y - SY(14);

    s_chart = lv_chart_create(s_modal);
    lv_obj_set_size(s_chart, LV_HOR_RES - SX(32), chart_h);
    lv_obj_set_pos(s_chart, SX(16), chart_y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 5, 8);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_radius(s_chart, 12, 0);
    lv_obj_set_style_pad_all(s_chart, SX(10), 0);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_CIRCULAR);

    s_series = lv_chart_add_series(s_chart, lv_color_hex(0x16a34a), LV_CHART_AXIS_PRIMARY_Y);

    /* Min and Max labels on chart overlay */
    s_lbl_hi = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_hi, "");
    lv_obj_set_style_text_font(s_lbl_hi, SF(14), 0);
    lv_obj_set_style_text_color(s_lbl_hi, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_pos(s_lbl_hi, SX(28), chart_y + SY(8));

    s_lbl_lo = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_lo, "");
    lv_obj_set_style_text_font(s_lbl_lo, SF(14), 0);
    lv_obj_set_style_text_color(s_lbl_lo, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_pos(s_lbl_lo, SX(28), chart_y + chart_h - SY(24));

    /* Load state & render */
    load_state_and_refresh();

    return s_modal;
}
