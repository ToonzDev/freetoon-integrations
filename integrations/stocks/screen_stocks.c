/*
 * screen_stocks.c — FreeToon Stock Exchange & AEX Interactive Screen
 *
 * Full-screen stock quote and interactive chart viewer.
 * Features:
 *   - Live quote or closing rate, day change percentage and points, day range, 52-week high/low.
 *   - Interactive multi-timeframe chart: 1D, 5D, 1M, 6M, 1J.
 *   - Dedicated, non-overlapping button bars:
 *       * Row 2: Favorites stock chips with dynamic configuration.
 *       * Row 3: Range metadata + timeframe selector chips.
 *       * Row 4: Dedicated, visible Y-axis values with 5 clean price levels.
 *   - Configurable favorites on the button row: add/remove any AEX stock directly
 *     from the picker modal.
 *   - Instant cached quote & closing rate display (no "Ophalen..." delays for cached tickers).
 *   - Background 400ms UI timer to seamlessly refresh when daemon finishes fetching.
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
#define MAX_FAVS        12

typedef struct {
    char ticker[32];
    char name[48];
} stock_preset_t;

static const stock_preset_t s_all_presets[] = {
    /* Nederlandse Indices */
    { "^AEX",        "AEX-Index" },
    { "^AMX",        "AMX Midcap" },
    { "^ASCX",       "AScX Smallcap" },

    /* De 25 AEX Hoofdfonds Aandelen */
    { "ABN.AS",      "ABN AMRO" },
    { "ADYEN.AS",    "Adyen" },
    { "AGN.AS",      "Aegon" },
    { "AD.AS",       "Ahold Delhaize" },
    { "AKZA.AS",     "AkzoNobel" },
    { "ASM.AS",      "ASM International" },
    { "ASML.AS",     "ASML" },
    { "ASRNL.AS",    "ASR Nederland" },
    { "BESI.AS",     "Besi" },
    { "DSFIR.AS",    "DSM-Firmenich" },
    { "EXO.AS",      "Exor" },
    { "GLPG.AS",     "Galapagos" },
    { "HEIA.AS",     "Heineken" },
    { "IMCD.AS",     "IMCD" },
    { "INGA.AS",     "ING Groep" },
    { "JDEP.AS",     "JDE Peet's" },
    { "TKWY.AS",     "Just Eat Takeaway" },
    { "KPN.AS",      "KPN" },
    { "NN.AS",       "NN Group" },
    { "PHIA.AS",     "Philips" },
    { "PRX.AS",      "Prosus" },
    { "RAND.AS",     "Randstad" },
    { "REN.AS",      "RELX" },
    { "SHELL.AS",    "Shell" },
    { "UMG.AS",      "Universal Music" },
    { "UNA.AS",      "Unilever" },
    { "WKL.AS",      "Wolters Kluwer" },

    /* Populaire AMX / Lokale Fondsen */
    { "ARCAD.AS",    "Arcadis" },
    { "FLOW.AS",     "Flow Traders" },
    { "ALFEN.AS",    "Alfen" },
    { "CORB.AS",     "Corbion" },

    /* Wereldwijd & Tech */
    { "^GSPC",       "S&P 500" },
    { "^IXIC",       "Nasdaq 100" },
    { "^DJI",        "Dow Jones" },
    { "^GDAXI",      "DAX 40" },
    { "NVDA",        "Nvidia" },
    { "ASML",        "ASML (US)" },
    { "AAPL",        "Apple" },
    { "MSFT",        "Microsoft" },
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

static lv_obj_t * s_fav_cont = NULL;
static lv_obj_t * s_fav_btns[MAX_FAVS];
static lv_obj_t * s_btn_more = NULL;

static lv_obj_t * s_tf_btns[5];
static const char * const s_tf_keys[5] = { "1d", "5d", "1mo", "6mo", "1y" };
static const char * const s_tf_labels[5] = { "1D", "5D", "1M", "6M", "1J" };
static int s_cur_tf = 0; /* default: 1D */

static lv_obj_t * s_chart = NULL;
static lv_chart_series_t * s_series = NULL;
static lv_obj_t * s_lbl_y[5]; /* 5 clean Y-axis values matching 5 grid lines */
static lv_obj_t * s_lbl_x[5]; /* 5 clean X-axis time/date labels under the chart */
static const char * const s_nl_days[7] = { "zo", "ma", "di", "wo", "do", "vr", "za" };
static const char * const s_nl_months[12] = { "jan", "feb", "mrt", "apr", "mei", "jun", "jul", "aug", "sep", "okt", "nov", "dec" };

static char s_cur_ticker[32] = "^AEX";
static char s_cur_name[64] = "AEX-Index";

/* Timer for background update check */
static lv_timer_t * s_poll_timer = NULL;
static long s_last_rendered_mtime = 0;

/* Modal picker elements */
static lv_obj_t * s_picker_modal = NULL;
static lv_obj_t * s_picker_favs_cont = NULL;
static lv_obj_t * s_picker_grid_cont = NULL;
static lv_obj_t * s_picker_ta = NULL;
static lv_obj_t * s_picker_kb = NULL;

static void load_state_and_refresh(void);
static void open_picker_modal(void);
static void close_picker_modal(void);
static void rebuild_fav_bar(void);
static void save_config(void);

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

static int is_in_favs(const char * ticker) {
    for (int i = 0; i < s_fav_count; i++) {
        if (strcasecmp(s_favs[i].ticker, ticker) == 0) return 1;
    }
    return 0;
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
        snprintf(s_favs[6].ticker, sizeof(s_favs[6].ticker), "ADYEN.AS");
        snprintf(s_favs[6].name, sizeof(s_favs[6].name), "Adyen");
        s_fav_count = 7;
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
        } else if (strncmp(line, "TIMEFRAME=", 10) == 0) {
            for (int k = 0; k < 5; k++) {
                if (strcmp(line + 10, s_tf_keys[k]) == 0) { s_cur_tf = k; break; }
            }
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

static void save_config(void) {
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
}

static void save_selected_ticker(const char * ticker, const char * name) {
    snprintf(s_cur_ticker, sizeof(s_cur_ticker), "%s", ticker);
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", name);
    save_config();

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
        save_config();
        /* Signal daemon if timeframe changed */
        FILE * trg = fopen(TRIG_PATH, "w");
        if (trg) {
            fprintf(trg, "1\n");
            fclose(trg);
        }
        load_state_and_refresh();
    }
}

static void on_more_click(lv_event_t * e) {
    (void)e;
    open_picker_modal();
}

static void on_modal_deleted(lv_event_t * e) {
    (void)e;
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    s_modal = NULL;
    s_chart = NULL;
    s_series = NULL;
    for (int i = 0; i < 5; i++) {
        s_lbl_y[i] = NULL;
        s_lbl_x[i] = NULL;
    }
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

/* Format clean price in Dutch notation */
static void format_price_num(double val, char * out, size_t outsz) {
    long int_part = (long)fabs(val);
    int dec_part = (int)(fabs(val - int_part) * 100.0 + 0.5);
    if (dec_part >= 100) { int_part++; dec_part = 0; }

    if (int_part >= 1000) {
        long thousands = int_part / 1000;
        long hundreds = int_part % 1000;
        snprintf(out, outsz, "%ld.%03ld,%02d", thousands, hundreds, dec_part);
    } else {
        snprintf(out, outsz, "%ld,%02d", int_part, dec_part);
    }
}

/* Calculate clean quantized nice-step Y-axis bounds (like screen_stats in FreeToon) */
static void calculate_nice_stock_range(double min_p, double max_p, double * out_bottom, double * out_step, double * out_top) {
    if (max_p - min_p < 0.001) {
        max_p = min_p + 1.0;
    }
    double raw_span = max_p - min_p;
    raw_span *= 1.05; /* 5% headroom so data points do not touch chart borders */
    double raw_step = raw_span / 4.0;

    double exponent = floor(log10(raw_step));
    double mag = pow(10.0, exponent);
    double frac = raw_step / mag;

    const double nice_fracs[] = { 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 7.5, 8.0, 10.0, 12.5, 15.0, 20.0, 25.0 };
    int num_fracs = (int)(sizeof(nice_fracs) / sizeof(nice_fracs[0]));

    double step = nice_fracs[0] * mag;
    double bottom = 0, top = 0;

    for (int i = 0; i < num_fracs; i++) {
        if (nice_fracs[i] < frac && i < num_fracs - 1) continue;
        step = nice_fracs[i] * mag;

        bottom = floor(min_p / step) * step;
        top = bottom + 4.0 * step;

        if (bottom <= min_p && top >= max_p) {
            break;
        }
        if ((bottom - step) <= min_p && ((bottom - step) + 4.0 * step) >= max_p) {
            bottom -= step;
            top = bottom + 4.0 * step;
            break;
        }
    }

    *out_bottom = bottom;
    *out_step = step;
    *out_top = top;
}

/* Format clean tick with smart decimal rounding based on step */
static void format_nice_tick(double val, double step, char * out, size_t outsz) {
    int has_cents = (step < 1.0) || (fabs(step - floor(step + 0.0001)) > 0.005);
    long int_part = (long)floor(val + 0.0001);

    if (!has_cents) {
        if (int_part >= 1000) {
            snprintf(out, outsz, "%ld.%03ld", int_part / 1000, int_part % 1000);
        } else {
            snprintf(out, outsz, "%ld", int_part);
        }
    } else {
        int dec = (int)(fabs(val - int_part) * 100.0 + 0.5);
        if (dec >= 100) { int_part++; dec = 0; }
        if (int_part >= 1000) {
            snprintf(out, outsz, "%ld.%03ld,%02d", int_part / 1000, int_part % 1000, dec);
        } else {
            snprintf(out, outsz, "%ld,%02d", int_part, dec);
        }
    }
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

    /* 1. Try to read active state.json or cached state_<safe_ticker>.json */
    char safe_tk[64];
    make_safe_ticker(s_cur_ticker, safe_tk, sizeof(safe_tk));

    char buf[4096];
    char val[32] = "", sub[32] = "", name[64] = "", ticker[32] = "";
    double change = 0.0, day_hi = 0.0, day_lo = 0.0, hi_52w = 0.0, lo_52w = 0.0;
    int have_data = 0;

    /* Check state.json */
    FILE * f = fopen(STATE_PATH, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);

        extract_json_str(buf, "ticker", ticker, sizeof(ticker));
        if (strcmp(ticker, s_cur_ticker) == 0) {
            extract_json_str(buf, "name", name, sizeof(name));
            extract_json_str(buf, "value", val, sizeof(val));
            extract_json_str(buf, "subtitle", sub, sizeof(sub));
            extract_json_num(buf, "change", &change);
            extract_json_num(buf, "day_high", &day_hi);
            extract_json_num(buf, "day_low", &day_lo);
            extract_json_num(buf, "high_52w", &hi_52w);
            extract_json_num(buf, "low_52w", &lo_52w);
            have_data = 1;
        }
    }

    /* If state.json is not for current ticker, look for per-ticker cache */
    if (!have_data) {
        char cache_path[256];
        snprintf(cache_path, sizeof(cache_path), "%s/state_%s.json", BASE_DIR, safe_tk);
        FILE * fc = fopen(cache_path, "r");
        if (fc) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, fc);
            buf[n] = 0;
            fclose(fc);

            extract_json_str(buf, "ticker", ticker, sizeof(ticker));
            extract_json_str(buf, "name", name, sizeof(name));
            extract_json_str(buf, "value", val, sizeof(val));
            extract_json_str(buf, "subtitle", sub, sizeof(sub));
            extract_json_num(buf, "change", &change);
            extract_json_num(buf, "day_high", &day_hi);
            extract_json_num(buf, "day_low", &day_lo);
            extract_json_num(buf, "high_52w", &hi_52w);
            extract_json_num(buf, "low_52w", &lo_52w);
            have_data = 1;
        }
    }

    if (have_data) {
        char title_buf[96];
        snprintf(title_buf, sizeof(title_buf), "%s (%s)", name[0] ? name : s_cur_name, s_cur_ticker);
        lv_label_set_text(s_lbl_title, title_buf);
        lv_label_set_text(s_lbl_price, val[0] ? val : "...");
        lv_label_set_text(s_lbl_chg, sub[0] ? sub : "");

        uint32_t badge_col = (change >= 0) ? 0x16a34a : 0xdc2626;
        lv_obj_set_style_bg_color(s_badge_chg, lv_color_hex(badge_col), 0);

        char info_buf[128];
        if (day_hi > 0.01 && hi_52w > 0.01) {
            char dhi[24], dlo[24], h52[24], l52[24];
            format_price_num(day_hi, dhi, sizeof(dhi));
            format_price_num(day_lo, dlo, sizeof(dlo));
            format_price_num(hi_52w, h52, sizeof(h52));
            format_price_num(lo_52w, l52, sizeof(l52));
            snprintf(info_buf, sizeof(info_buf), "Dag: %s - %s   |   52w: %s - %s",
                     dlo, dhi, l52, h52);
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
        lv_label_set_text(s_lbl_info, "Live koers wordt opgehaald...");
    }

    /* 2. Load TSV chart points */
    char tsv_path[256];
    snprintf(tsv_path, sizeof(tsv_path), "%s/%s_%s.tsv", HIST_DIR, safe_tk, s_tf_keys[s_cur_tf]);

    FILE * tf = fopen(tsv_path, "r");
    double prices[MAX_CHART_PTS];
    long timestamps[MAX_CHART_PTS];
    int pt_count = 0;
    double min_p = 1e9, max_p = -1e9;

    if (tf) {
        char tline[128];
        while (fgets(tline, sizeof(tline), tf) && pt_count < MAX_CHART_PTS) {
            char * tab = strchr(tline, '\t');
            if (tab) {
                long ts = strtol(tline, NULL, 10);
                double p = strtod(tab + 1, NULL);
                if (p > 0.0001) {
                    timestamps[pt_count] = ts;
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
        double nice_bottom = 0, nice_step = 0, nice_top = 0;
        calculate_nice_stock_range(min_p, max_p, &nice_bottom, &nice_step, &nice_top);

        /* Range in integer coordinates 0..1000 */
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);

        for (int i = 0; i < pt_count; i++) {
            int norm = (int)(((prices[i] - nice_bottom) / (nice_top - nice_bottom)) * 1000.0);
            if (norm < 0) norm = 0;
            if (norm > 1000) norm = 1000;
            lv_chart_set_next_value(s_chart, s_series, (lv_coord_t)norm);
        }

        /* Color green if end >= start, else red */
        uint32_t line_color = (prices[pt_count - 1] >= prices[0]) ? 0x16a34a : 0xdc2626;
        lv_chart_set_series_color(s_chart, s_series, lv_color_hex(line_color));

        /* 5 Clean Y-Axis Scale Values matching the 5 grid lines exactly (top to bottom) */
        for (int y = 0; y < 5; y++) {
            if (!s_lbl_y[y]) continue;
            double v = nice_top - (double)y * nice_step;
            char y_buf[48];
            format_nice_tick(v, nice_step, y_buf, sizeof(y_buf));
            lv_label_set_text(s_lbl_y[y], y_buf);
        }

        /* 5 Clean X-Axis Time/Date Labels across the time range */
        for (int k = 0; k < 5; k++) {
            if (!s_lbl_x[k]) continue;
            if (timestamps[0] <= 0) {
                lv_label_set_text(s_lbl_x[k], "");
                continue;
            }
            int idx = (k * (pt_count - 1)) / 4;
            time_t t = timestamps[idx];
            struct tm tm_pt;
            localtime_r(&t, &tm_pt);
            char xbuf[32];

            if (s_cur_tf == 0) {
                /* 1D: Hour:Minute (e.g. 09:05, 11:15, 13:30, ...) */
                strftime(xbuf, sizeof(xbuf), "%H:%M", &tm_pt);
            } else if (s_cur_tf == 1) {
                /* 5D: Day name + date (e.g. ma 3, di 4, wo 5, ...) */
                snprintf(xbuf, sizeof(xbuf), "%s %d",
                         s_nl_days[tm_pt.tm_wday % 7], tm_pt.tm_mday);
            } else if (s_cur_tf == 2) {
                /* 1M: Day + month (e.g. 10 okt, 17 okt, ...) */
                snprintf(xbuf, sizeof(xbuf), "%d %s",
                         tm_pt.tm_mday, s_nl_months[tm_pt.tm_mon % 12]);
            } else if (s_cur_tf == 3) {
                /* 6M: Month name (e.g. mei, jul, sep, nov) */
                snprintf(xbuf, sizeof(xbuf), "%s",
                         s_nl_months[tm_pt.tm_mon % 12]);
            } else {
                /* 1J: Month + short year (e.g. nov '25, feb '26, ...) */
                snprintf(xbuf, sizeof(xbuf), "%s '%02d",
                         s_nl_months[tm_pt.tm_mon % 12], tm_pt.tm_year % 100);
            }
            lv_label_set_text(s_lbl_x[k], xbuf);
        }
    } else {
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        for (int i = 0; i < 10; i++) {
            lv_chart_set_next_value(s_chart, s_series, 50);
        }
        for (int y = 0; y < 5; y++) {
            if (s_lbl_y[y]) lv_label_set_text(s_lbl_y[y], "");
        }
        for (int k = 0; k < 5; k++) {
            if (s_lbl_x[k]) lv_label_set_text(s_lbl_x[k], "");
        }
    }
    lv_chart_refresh(s_chart);
}

/* Background polling timer to auto-refresh when daemon finishes */
static void on_poll_timer(lv_timer_t * timer) {
    (void)timer;
    if (!s_modal) return;

    struct stat st;
    if (stat(STATE_PATH, &st) == 0) {
        if (st.st_mtime != s_last_rendered_mtime) {
            s_last_rendered_mtime = st.st_mtime;
            load_state_and_refresh();
        }
    }
}

/* ===================================================================== */
/* Favorite Buttons Bar Rendering                                        */
/* ===================================================================== */
static void rebuild_fav_bar(void) {
    if (!s_fav_cont) return;
    lv_obj_clean(s_fav_cont);

    for (int i = 0; i < s_fav_count && i < MAX_FAVS; i++) {
        s_fav_btns[i] = lv_btn_create(s_fav_cont);
        lv_obj_set_size(s_fav_btns[i], SX(80), SY(34));
        int is_active = (strcmp(s_favs[i].ticker, s_cur_ticker) == 0);
        lv_obj_set_style_bg_color(s_fav_btns[i], lv_color_hex(is_active ? THEME_BTN_ACCENT : THEME_BTN_BG), 0);
        lv_obj_set_style_radius(s_fav_btns[i], 8, 0);
        lv_obj_set_style_pad_all(s_fav_btns[i], 0, 0);
        lv_obj_add_event_cb(s_fav_btns[i], on_fav_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(s_fav_btns[i]);
        lv_label_set_text(l, s_favs[i].name);
        lv_obj_set_style_text_font(l, SF(13), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(l);
    }

    /* "+ Kiezen" button */
    s_btn_more = lv_btn_create(s_fav_cont);
    lv_obj_set_size(s_btn_more, SX(92), SY(34));
    lv_obj_set_style_bg_color(s_btn_more, lv_color_hex(0x27272a), 0);
    lv_obj_set_style_border_color(s_btn_more, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_more, 1, 0);
    lv_obj_set_style_radius(s_btn_more, 8, 0);
    lv_obj_set_style_pad_all(s_btn_more, 0, 0);
    lv_obj_add_event_cb(s_btn_more, on_more_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lm = lv_label_create(s_btn_more);
    lv_label_set_text(lm, "+ Kiezen");
    lv_obj_set_style_text_font(lm, SF(13), 0);
    lv_obj_set_style_text_color(lm, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(lm);
}

/* ===================================================================== */
/* Stock Picker Modal (Favorites Management & All AEX Presets)           */
/* ===================================================================== */

static void render_picker_favs(void);
static void render_picker_grid(void);

static void on_picker_remove_fav(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_fav_count) {
        for (int i = idx; i < s_fav_count - 1; i++) {
            s_favs[i] = s_favs[i + 1];
        }
        s_fav_count--;
        save_config();
        rebuild_fav_bar();
        render_picker_favs();
        render_picker_grid();
    }
}

static void on_picker_toggle_fav(lv_event_t * e) {
    int p_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (p_idx < 0 || p_idx >= (int)NUM_ALL_PRESETS) return;

    const stock_preset_t * p = &s_all_presets[p_idx];
    int fav_idx = -1;
    for (int i = 0; i < s_fav_count; i++) {
        if (strcasecmp(s_favs[i].ticker, p->ticker) == 0) {
            fav_idx = i;
            break;
        }
    }

    if (fav_idx >= 0) {
        /* Remove from favs */
        for (int i = fav_idx; i < s_fav_count - 1; i++) {
            s_favs[i] = s_favs[i + 1];
        }
        s_fav_count--;
    } else {
        /* Add to favs if space available */
        if (s_fav_count < MAX_FAVS) {
            snprintf(s_favs[s_fav_count].ticker, sizeof(s_favs[0].ticker), "%s", p->ticker);
            snprintf(s_favs[s_fav_count].name, sizeof(s_favs[0].name), "%s", p->name);
            s_fav_count++;
        }
    }

    save_config();
    rebuild_fav_bar();
    render_picker_favs();
    render_picker_grid();
}

static void on_picker_select_stock(lv_event_t * e) {
    int p_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (p_idx >= 0 && p_idx < (int)NUM_ALL_PRESETS) {
        save_selected_ticker(s_all_presets[p_idx].ticker, s_all_presets[p_idx].name);
        close_picker_modal();
        load_state_and_refresh();
    }
}

static void on_picker_custom_action(int add_to_fav) {
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
    if (!cleaned[0]) return;

    /* Smart Euronext Amsterdam (.AS) resolution if ticker has no dot and no ^ */
    char resolved_ticker[32];
    char resolved_name[48];
    snprintf(resolved_ticker, sizeof(resolved_ticker), "%s", cleaned);
    snprintf(resolved_name, sizeof(resolved_name), "%s", cleaned);

    if (!strchr(cleaned, '.') && cleaned[0] != '^') {
        for (size_t i = 0; i < NUM_ALL_PRESETS; i++) {
            const char * dot = strchr(s_all_presets[i].ticker, '.');
            if (dot) {
                size_t pfx = dot - s_all_presets[i].ticker;
                if (strlen(cleaned) == pfx && strncasecmp(cleaned, s_all_presets[i].ticker, pfx) == 0) {
                    snprintf(resolved_ticker, sizeof(resolved_ticker), "%s", s_all_presets[i].ticker);
                    snprintf(resolved_name, sizeof(resolved_name), "%s", s_all_presets[i].name);
                    break;
                }
            } else if (strcasecmp(cleaned, s_all_presets[i].name) == 0) {
                snprintf(resolved_ticker, sizeof(resolved_ticker), "%s", s_all_presets[i].ticker);
                snprintf(resolved_name, sizeof(resolved_name), "%s", s_all_presets[i].name);
                break;
            }
        }
    }

    if (add_to_fav && s_fav_count < MAX_FAVS && !is_in_favs(resolved_ticker)) {
        snprintf(s_favs[s_fav_count].ticker, sizeof(s_favs[0].ticker), "%s", resolved_ticker);
        snprintf(s_favs[s_fav_count].name, sizeof(s_favs[0].name), "%s", resolved_name);
        s_fav_count++;
        save_config();
        rebuild_fav_bar();
        render_picker_favs();
        render_picker_grid();
    }

    save_selected_ticker(resolved_ticker, resolved_name);
    close_picker_modal();
    load_state_and_refresh();
}

static void on_picker_btn_view(lv_event_t * e) {
    (void)e;
    on_picker_custom_action(0);
}

static void on_picker_btn_add(lv_event_t * e) {
    (void)e;
    on_picker_custom_action(1);
}

static void on_picker_kb_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_picker_custom_action(0);
    } else if (code == LV_EVENT_CANCEL) {
        close_picker_modal();
    }
}

static void close_picker_modal(void) {
    if (s_picker_modal) {
        lv_obj_del(s_picker_modal);
        s_picker_modal = NULL;
        s_picker_favs_cont = NULL;
        s_picker_grid_cont = NULL;
        s_picker_ta = NULL;
        s_picker_kb = NULL;
    }
}

static void on_picker_close_click(lv_event_t * e) {
    (void)e;
    close_picker_modal();
}

static void render_picker_favs(void) {
    if (!s_picker_favs_cont) return;
    lv_obj_clean(s_picker_favs_cont);

    for (int i = 0; i < s_fav_count; i++) {
        lv_obj_t * b = lv_btn_create(s_picker_favs_cont);
        lv_obj_set_size(b, SX(112), SY(32));
        lv_obj_set_style_bg_color(b, lv_color_hex(0x27272a), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_picker_remove_fav, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text_fmt(l, "%s  #dc2626 " LV_SYMBOL_CLOSE "#", s_favs[i].name);
        lv_label_set_recolor(l, true);
        lv_obj_set_style_text_font(l, SF(12), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(l);
    }
}

static void render_picker_grid(void) {
    if (!s_picker_grid_cont) return;
    lv_obj_clean(s_picker_grid_cont);

    for (size_t i = 0; i < NUM_ALL_PRESETS; i++) {
        int in_fav = is_in_favs(s_all_presets[i].ticker);

        lv_obj_t * card = lv_obj_create(s_picker_grid_cont);
        lv_obj_set_size(card, SX(174), SY(46));
        lv_obj_set_style_bg_color(card, lv_color_hex(0x222226), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(in_fav ? 0x16a34a : THEME_BORDER), 0);
        lv_obj_set_style_border_width(card, in_fav ? 2 : 1, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, SX(4), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* Main click area to select stock */
        lv_obj_t * b_sel = lv_btn_create(card);
        lv_obj_set_size(b_sel, SX(116), SY(36));
        lv_obj_align(b_sel, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(b_sel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(b_sel, 0, 0);
        lv_obj_add_event_cb(b_sel, on_picker_select_stock, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l_name = lv_label_create(b_sel);
        lv_label_set_text(l_name, s_all_presets[i].name);
        lv_obj_set_style_text_font(l_name, SF(13), 0);
        lv_obj_set_style_text_color(l_name, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_align(l_name, LV_ALIGN_TOP_LEFT, SX(2), SY(2));

        lv_obj_t * l_tk = lv_label_create(b_sel);
        lv_label_set_text(l_tk, s_all_presets[i].ticker);
        lv_obj_set_style_text_font(l_tk, SF(11), 0);
        lv_obj_set_style_text_color(l_tk, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_align(l_tk, LV_ALIGN_BOTTOM_LEFT, SX(2), SY(-2));

        /* Toggle Favorite Button */
        lv_obj_t * b_fav = lv_btn_create(card);
        lv_obj_set_size(b_fav, SX(44), SY(34));
        lv_obj_align(b_fav, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(b_fav, lv_color_hex(in_fav ? 0x16a34a : 0x334155), 0);
        lv_obj_set_style_radius(b_fav, 6, 0);
        lv_obj_set_style_pad_all(b_fav, 0, 0);
        lv_obj_add_event_cb(b_fav, on_picker_toggle_fav, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l_fav = lv_label_create(b_fav);
        lv_label_set_text(l_fav, in_fav ? LV_SYMBOL_OK : "+");
        lv_obj_set_style_text_font(l_fav, SF(14), 0);
        lv_obj_set_style_text_color(l_fav, lv_color_hex(0xffffff), 0);
        lv_obj_center(l_fav);
    }
}

static void open_picker_modal(void) {
    if (s_picker_modal) close_picker_modal();

    s_picker_modal = lv_obj_create(s_modal);
    lv_obj_set_size(s_picker_modal, LV_PCT(96), LV_PCT(94));
    lv_obj_center(s_picker_modal);
    lv_obj_set_style_bg_color(s_picker_modal, lv_color_hex(0x18181b), 0);
    lv_obj_set_style_border_color(s_picker_modal, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_picker_modal, 2, 0);
    lv_obj_set_style_radius(s_picker_modal, 14, 0);
    lv_obj_set_style_pad_all(s_picker_modal, SX(12), 0);

    /* Header */
    lv_obj_t * lbl_head = lv_label_create(s_picker_modal);
    lv_label_set_text(lbl_head, "Kies & Beheer AEX Aandelen");
    lv_obj_set_style_text_font(lbl_head, SF(18), 0);
    lv_obj_set_style_text_color(lbl_head, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_align(lbl_head, LV_ALIGN_TOP_LEFT, SX(6), SY(2));

    /* Close Button */
    lv_obj_t * btn_cls = lv_btn_create(s_picker_modal);
    lv_obj_set_size(btn_cls, SX(46), SY(34));
    lv_obj_align(btn_cls, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_cls, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_cls, 8, 0);
    lv_obj_add_event_cb(btn_cls, on_picker_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l_cls = lv_label_create(btn_cls);
    lv_label_set_text(l_cls, LV_SYMBOL_CLOSE);
    lv_obj_center(l_cls);

    /* Section 1: Active Favorite Buttons on Bar */
    lv_obj_t * lbl_favs = lv_label_create(s_picker_modal);
    lv_label_set_text(lbl_favs, "Knoppenbalk favorieten (tik om te verwijderen):");
    lv_obj_set_style_text_font(lbl_favs, SF(12), 0);
    lv_obj_set_style_text_color(lbl_favs, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_pos(lbl_favs, SX(6), SY(36));

    s_picker_favs_cont = lv_obj_create(s_picker_modal);
    lv_obj_set_size(s_picker_favs_cont, LV_PCT(100), SY(42));
    lv_obj_set_pos(s_picker_favs_cont, 0, SY(54));
    lv_obj_set_style_bg_color(s_picker_favs_cont, lv_color_hex(0x18181b), 0);
    lv_obj_set_style_border_width(s_picker_favs_cont, 0, 0);
    lv_obj_set_style_pad_all(s_picker_favs_cont, SX(2), 0);
    lv_obj_set_flex_flow(s_picker_favs_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollbar_mode(s_picker_favs_cont, LV_SCROLLBAR_MODE_OFF);
    render_picker_favs();

    /* Section 2: Custom Ticker Input */
    s_picker_ta = lv_textarea_create(s_picker_modal);
    lv_obj_set_size(s_picker_ta, SX(240), SY(36));
    lv_obj_set_pos(s_picker_ta, SX(6), SY(102));
    lv_textarea_set_one_line(s_picker_ta, true);
    lv_textarea_set_placeholder_text(s_picker_ta, "Ticker (bijv. ASML, AAPL)...");
    lv_obj_set_style_bg_color(s_picker_ta, lv_color_hex(0x27272a), 0);
    lv_obj_set_style_text_color(s_picker_ta, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_border_color(s_picker_ta, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_radius(s_picker_ta, 8, 0);

    lv_obj_t * b_view = lv_btn_create(s_picker_modal);
    lv_obj_set_size(b_view, SX(96), SY(36));
    lv_obj_set_pos(b_view, SX(252), SY(102));
    lv_obj_set_style_bg_color(b_view, lv_color_hex(THEME_BTN_ACCENT), 0);
    lv_obj_set_style_radius(b_view, 8, 0);
    lv_obj_add_event_cb(b_view, on_picker_btn_view, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lv = lv_label_create(b_view);
    lv_label_set_text(lv, "Bekijken");
    lv_obj_center(lv);

    lv_obj_t * b_add = lv_btn_create(s_picker_modal);
    lv_obj_set_size(b_add, SX(96), SY(36));
    lv_obj_set_pos(b_add, SX(354), SY(102));
    lv_obj_set_style_bg_color(b_add, lv_color_hex(0x16a34a), 0);
    lv_obj_set_style_radius(b_add, 8, 0);
    lv_obj_add_event_cb(b_add, on_picker_btn_add, LV_EVENT_CLICKED, NULL);
    lv_obj_t * la = lv_label_create(b_add);
    lv_label_set_text(la, "+ Aan balk");
    lv_obj_center(la);

    /* Section 3: All AEX & Global Presets Grid */
    int cont_y = SY(146);
    int cont_h = (LV_VER_RES < 600) ? SY(150) : SY(180);

    s_picker_grid_cont = lv_obj_create(s_picker_modal);
    lv_obj_set_size(s_picker_grid_cont, LV_PCT(100), cont_h);
    lv_obj_set_pos(s_picker_grid_cont, 0, cont_y);
    lv_obj_set_style_bg_color(s_picker_grid_cont, lv_color_hex(0x18181b), 0);
    lv_obj_set_style_border_width(s_picker_grid_cont, 0, 0);
    lv_obj_set_style_pad_all(s_picker_grid_cont, SX(4), 0);
    lv_obj_set_flex_flow(s_picker_grid_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_picker_grid_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    render_picker_grid();

    /* On-Screen Virtual Keyboard */
    s_picker_kb = lv_keyboard_create(s_picker_modal);
    int kb_h = (LV_VER_RES < 600) ? SY(120) : SY(150);
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

    /* ================================================================= */
    /* ROW 1: Header (Title, Price, Change Badge, Refresh, Close)        */
    /* ================================================================= */
    int x_w = (LV_VER_RES < 600) ? 52 : 60;
    int x_h = (LV_VER_RES < 600) ? 38 : 44;

    /* Close Button */
    lv_obj_t * b_close = lv_btn_create(s_modal);
    lv_obj_set_size(b_close, x_w, x_h);
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(b_close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_close, 10, 0);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_close = lv_label_create(b_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_close, SF(18), 0);
    lv_obj_center(lbl_close);

    /* Refresh Button */
    lv_obj_t * b_ref = lv_btn_create(s_modal);
    lv_obj_set_size(b_ref, x_w, x_h);
    lv_obj_align(b_ref, LV_ALIGN_TOP_RIGHT, SX(-12) - x_w - SX(8), SY(8));
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_ref, 10, 0);
    lv_obj_add_event_cb(b_ref, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_ref = lv_label_create(b_ref);
    lv_label_set_text(lbl_ref, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_ref, SF(16), 0);
    lv_obj_center(lbl_ref);

    /* Stock Name / Symbol */
    s_lbl_title = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_title, s_cur_name);
    lv_obj_set_style_text_font(s_lbl_title, SF(20), 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_pos(s_lbl_title, SX(16), SY(8));

    /* Current Price Label */
    s_lbl_price = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_price, "...");
    const lv_font_t * pf = (LV_VER_RES < 600) ? SF(28) : SF(34);
    lv_obj_set_style_text_font(s_lbl_price, pf, 0);
    lv_obj_set_style_text_color(s_lbl_price, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_pos(s_lbl_price, SX(16), SY(32));

    /* Change Badge Pill */
    s_badge_chg = lv_obj_create(s_modal);
    lv_obj_set_size(s_badge_chg, SX(165), SY(30));
    lv_obj_set_pos(s_badge_chg, SX(240), SY(36));
    lv_obj_set_style_bg_color(s_badge_chg, lv_color_hex(0x16a34a), 0);
    lv_obj_set_style_radius(s_badge_chg, 15, 0);
    lv_obj_set_style_border_width(s_badge_chg, 0, 0);
    lv_obj_set_style_pad_all(s_badge_chg, 0, 0);
    lv_obj_clear_flag(s_badge_chg, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_chg = lv_label_create(s_badge_chg);
    lv_label_set_text(s_lbl_chg, "...");
    lv_obj_set_style_text_font(s_lbl_chg, SF(15), 0);
    lv_obj_set_style_text_color(s_lbl_chg, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_lbl_chg);

    /* ================================================================= */
    /* ROW 2: Dedicated Stock Favorites Bar (Never Overlaps!)            */
    /* ================================================================= */
    int row2_y = SY(72);
    s_fav_cont = lv_obj_create(s_modal);
    lv_obj_set_size(s_fav_cont, LV_HOR_RES - SX(32), SY(40));
    lv_obj_set_pos(s_fav_cont, SX(16), row2_y);
    lv_obj_set_style_bg_color(s_fav_cont, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(s_fav_cont, 0, 0);
    lv_obj_set_style_pad_all(s_fav_cont, 0, 0);
    lv_obj_set_flex_flow(s_fav_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_fav_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_fav_cont, LV_SCROLLBAR_MODE_OFF);
    rebuild_fav_bar();

    /* ================================================================= */
    /* ROW 3: Secondary Range Info (Left) + Timeframe Chips (Right)      */
    /* ================================================================= */
    int row3_y = SY(114);

    s_lbl_info = lv_label_create(s_modal);
    lv_label_set_text(s_lbl_info, "...");
    lv_obj_set_style_text_font(s_lbl_info, SF(13), 0);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_pos(s_lbl_info, SX(16), row3_y + SY(8));

    /* Timeframe Selector Chips Row (1D, 5D, 1M, 6M, 1J) — Extra Large for Easy Touch */
    int tf_chip_w = SX(64);
    int tf_chip_h = SY(38);
    int tf_gap = SX(6);
    int tf_start_x = LV_HOR_RES - SX(16) - 5 * (tf_chip_w + tf_gap);

    for (int i = 0; i < 5; i++) {
        s_tf_btns[i] = lv_btn_create(s_modal);
        lv_obj_set_size(s_tf_btns[i], tf_chip_w, tf_chip_h);
        lv_obj_set_pos(s_tf_btns[i], tf_start_x + i * (tf_chip_w + tf_gap), row3_y);
        lv_obj_set_style_bg_color(s_tf_btns[i], lv_color_hex(THEME_BTN_BG), 0);
        lv_obj_set_style_radius(s_tf_btns[i], 8, 0);
        lv_obj_set_style_pad_all(s_tf_btns[i], 0, 0);
        lv_obj_add_event_cb(s_tf_btns[i], on_tf_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * l = lv_label_create(s_tf_btns[i]);
        lv_label_set_text(l, s_tf_labels[i]);
        lv_obj_set_style_text_font(l, SF(16), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(l);
    }

    /* ================================================================= */
    /* ROW 4: Chart + Dedicated Clear Y-Axis Values & X-Axis Labels      */
    /* ================================================================= */
    int chart_y = row3_y + SY(44);
    int x_axis_h = SY(22);
    int chart_h = LV_VER_RES - chart_y - x_axis_h - SY(10);
    int y_axis_w = SX(74);

    /* 5 Y-Axis Labels aligned with the 5 horizontal grid lines */
    for (int y = 0; y < 5; y++) {
        s_lbl_y[y] = lv_label_create(s_modal);
        lv_label_set_text(s_lbl_y[y], "");
        lv_obj_set_style_text_font(s_lbl_y[y], SF(12), 0);
        lv_obj_set_style_text_color(s_lbl_y[y], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_align(s_lbl_y[y], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(s_lbl_y[y], y_axis_w, SY(16));

        int pos_y;
        if (y == 0) pos_y = chart_y + SY(2);
        else if (y == 4) pos_y = chart_y + chart_h - SY(18);
        else pos_y = chart_y + (int)((chart_h - SY(16)) * (y / 4.0));

        lv_obj_set_pos(s_lbl_y[y], SX(8), pos_y);
    }

    /* Chart Container & lv_chart */
    int chart_x = SX(12) + y_axis_w + SX(4);
    int chart_w = LV_HOR_RES - chart_x - SX(16);

    s_chart = lv_chart_create(s_modal);
    lv_obj_set_size(s_chart, chart_w, chart_h);
    lv_obj_set_pos(s_chart, chart_x, chart_y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 5, 5);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_radius(s_chart, 12, 0);
    lv_obj_set_style_pad_all(s_chart, SX(6), 0);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_CIRCULAR);

    s_series = lv_chart_add_series(s_chart, lv_color_hex(0x16a34a), LV_CHART_AXIS_PRIMARY_Y);

    /* 5 Proper X-Axis Date/Time Labels under the chart */
    int x_label_y = chart_y + chart_h + SY(4);
    int lbl_w = SX(80);
    for (int k = 0; k < 5; k++) {
        s_lbl_x[k] = lv_label_create(s_modal);
        lv_label_set_text(s_lbl_x[k], "");
        lv_obj_set_style_text_font(s_lbl_x[k], SF(12), 0);
        lv_obj_set_style_text_color(s_lbl_x[k], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_size(s_lbl_x[k], lbl_w, SY(18));

        if (k == 0) {
            lv_obj_set_style_text_align(s_lbl_x[k], LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_pos(s_lbl_x[k], chart_x + SX(4), x_label_y);
        } else if (k == 4) {
            lv_obj_set_style_text_align(s_lbl_x[k], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(s_lbl_x[k], chart_x + chart_w - lbl_w - SX(4), x_label_y);
        } else {
            lv_obj_set_style_text_align(s_lbl_x[k], LV_TEXT_ALIGN_CENTER, 0);
            int cx = chart_x + (int)((double)chart_w * (k / 4.0));
            lv_obj_set_pos(s_lbl_x[k], cx - (lbl_w / 2), x_label_y);
        }
    }

    /* Load state & render */
    load_state_and_refresh();

    /* Start 400ms background polling timer */
    s_poll_timer = lv_timer_create(on_poll_timer, 400, NULL);

    return s_modal;
}
