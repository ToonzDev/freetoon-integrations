/*
 * stocks_daemon.c — FreeToon Stock Exchange & AEX Companion Daemon
 *
 * Runs in background. Periodically fetches live stock quotes and historical
 * chart data (Yahoo Finance API) for the configured stock/index (default: ^AEX).
 * Persists data to state.json & TSV series, and notifies FreeToon HTTP API
 * on port 10081 to keep the home tile live.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>
#include <math.h>

#define BASE_DIR        "/mnt/data/integrations/stocks"
#define CONF_PATH       BASE_DIR "/stocks.conf"
#define HIST_DIR        BASE_DIR "/hist"
#define STATE_PATH      BASE_DIR "/state.json"
#define TRIG_PATH       BASE_DIR "/reload.trig"

#define POLL_INTERVAL_S 120   /* 2 minutes for quote & 1d chart */
#define HIST_INTERVAL_S 3600  /* 1 hour for 5d, 1mo, 6mo, 1y */

#define MAX_JSON_BUF    (128 * 1024)

typedef struct {
    char ticker[32];
    char name[64];
    char display_title[32];
    char currency[16];
    double price;
    double prev_close;
    double change;
    double change_pct;
    double day_high;
    double day_low;
    double high_52w;
    double low_52w;
    long market_time;
    char formatted_val[48];
    char formatted_sub[48];
} quote_data_t;

static volatile int g_running = 1;
static char g_curr_ticker[32] = "^AEX";
static char g_curr_name[64] = "AEX-Index";
static time_t g_conf_mtime = 0;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void make_safe_ticker(const char * src, char * dst, size_t maxlen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < maxlen; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == '^') {
            dst[j++] = 'X'; /* ^AEX -> XAEX */
        } else if (c == '.') {
            dst[j++] = '_'; /* ASML.AS -> ASML_AS */
        }
    }
    dst[j] = 0;
}

static void make_url_ticker(const char * src, char * dst, size_t maxlen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 3 < maxlen; i++) {
        if (src[i] == '^') {
            dst[j++] = '%';
            dst[j++] = '5';
            dst[j++] = 'E';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = 0;
}

static void ensure_conf_exists(void) {
    mkdir(BASE_DIR, 0755);
    mkdir(HIST_DIR, 0755);

    struct stat st;
    if (stat(CONF_PATH, &st) != 0) {
        FILE * f = fopen(CONF_PATH, "w");
        if (f) {
            fprintf(f, "TICKER=^AEX\n");
            fprintf(f, "NAME=AEX-Index\n");
            fprintf(f, "TIMEFRAME=1d\n");
            fprintf(f, "FAV=^AEX,AEX-Index\n");
            fprintf(f, "FAV=ASML.AS,ASML\n");
            fprintf(f, "FAV=SHELL.AS,Shell\n");
            fprintf(f, "FAV=INGA.AS,ING\n");
            fprintf(f, "FAV=AD.AS,Ahold Delhaize\n");
            fprintf(f, "FAV=HEIA.AS,Heineken\n");
            fprintf(f, "FAV=PHIA.AS,Philips\n");
            fprintf(f, "FAV=^GSPC,S&P 500\n");
            fclose(f);
        }
    }
}

static int read_conf(char * ticker, size_t tlen, char * name, size_t nlen) {
    FILE * f = fopen(CONF_PATH, "r");
    if (!f) return 0;

    struct stat st;
    if (fstat(fileno(f), &st) == 0) {
        g_conf_mtime = st.st_mtime;
    }

    char line[256];
    ticker[0] = 0;
    name[0] = 0;

    while (fgets(line, sizeof(line), f)) {
        char * nl = strchr(line, '\r'); if (nl) *nl = 0;
        nl = strchr(line, '\n'); if (nl) *nl = 0;

        if (strncmp(line, "TICKER=", 7) == 0) {
            snprintf(ticker, tlen, "%s", line + 7);
        } else if (strncmp(line, "NAME=", 5) == 0) {
            snprintf(name, nlen, "%s", line + 5);
        }
    }
    fclose(f);

    if (!ticker[0]) snprintf(ticker, tlen, "^AEX");
    if (!name[0]) {
        if (!strcmp(ticker, "^AEX")) snprintf(name, nlen, "AEX-Index");
        else snprintf(name, nlen, "%s", ticker);
    }
    return 1;
}

static int post_to_freetoon(const char * title, const char * val, const char * sub) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(10081);
    a.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }

    char body[512];
    int bn = snprintf(body, sizeof(body),
        "{\"title\":\"%s\",\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\"}",
        title && title[0] ? title : "AEX",
        val && val[0] ? val : "...",
        sub && sub[0] ? sub : "");

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/stocks/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10081\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bn, body);

    send(s, req, rn, 0);
    close(s);
    return 0;
}

/* Format number in Dutch notation (thousands separator '.', decimal separator ',') */
static void format_dutch_price(double val, int is_index, const char * currency, char * out, size_t outsz) {
    const char * sym = "";
    if (!is_index) {
        if (!strcasecmp(currency, "EUR")) sym = "\xe2\x82\xac "; /* € */
        else if (!strcasecmp(currency, "USD")) sym = "$ ";
        else if (!strcasecmp(currency, "GBP")) sym = "\xc2\xa3 "; /* £ */
        else sym = "";
    }

    long int_part = (long)fabs(val);
    int dec_part = (int)(fabs(val - int_part) * 100.0 + 0.5);
    if (dec_part >= 100) {
        int_part += 1;
        dec_part = 0;
    }

    if (int_part >= 1000) {
        long thousands = int_part / 1000;
        long hundreds = int_part % 1000;
        snprintf(out, outsz, "%s%ld.%03ld,%02d", sym, thousands, hundreds, dec_part);
    } else {
        snprintf(out, outsz, "%s%ld,%02d", sym, int_part, dec_part);
    }
}

static void format_dutch_change(double change, double change_pct, char * out, size_t outsz) {
    char sign = (change >= 0) ? '+' : '-';
    double abs_chg = fabs(change);
    double abs_pct = fabs(change_pct);

    long int_part = (long)abs_chg;
    int dec_part = (int)((abs_chg - int_part) * 100.0 + 0.5);
    if (dec_part >= 100) { int_part++; dec_part = 0; }

    long int_pct = (long)abs_pct;
    int dec_pct = (int)((abs_pct - int_pct) * 100.0 + 0.5);
    if (dec_pct >= 100) { int_pct++; dec_pct = 0; }

    snprintf(out, outsz, "%c%ld,%02d (%c%ld,%02d%%)",
             sign, int_part, dec_part, sign, int_pct, dec_pct);
}

static int parse_meta_num(const char * json, const char * key, double * out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char * p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) return 0;
    char * end = NULL;
    double d = strtod(p, &end);
    if (end != p) {
        *out = d;
        return 1;
    }
    return 0;
}

static int parse_meta_str(const char * json, const char * key, char * out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char * p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return (i > 0);
}

static int save_tsv_series(const char * safe_ticker, const char * tf, const char * json) {
    const char * p_ts = strstr(json, "\"timestamp\":[");
    if (!p_ts) return 0;
    p_ts += 13;

    const char * p_cl = strstr(json, "\"close\":[");
    if (!p_cl) return 0;
    p_cl += 9;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s_%s.tsv", HIST_DIR, safe_ticker, tf);
    FILE * f = fopen(path, "w");
    if (!f) return 0;

    int points = 0;
    while (*p_ts && *p_cl && *p_ts != ']' && *p_cl != ']') {
        while (*p_ts == ' ' || *p_ts == ',') p_ts++;
        while (*p_cl == ' ' || *p_cl == ',') p_cl++;
        if (*p_ts == ']' || *p_cl == ']') break;

        char * end_ts = NULL;
        long ts = strtol(p_ts, &end_ts, 10);
        if (end_ts == p_ts) break;
        p_ts = end_ts;

        if (strncmp(p_cl, "null", 4) == 0) {
            p_cl += 4;
            continue;
        }

        char * end_cl = NULL;
        double cl = strtod(p_cl, &end_cl);
        if (end_cl == p_cl) break;
        p_cl = end_cl;

        if (ts > 0 && cl > 0.0001) {
            fprintf(f, "%ld\t%.4f\n", ts, cl);
            points++;
        }
    }
    fclose(f);
    return points;
}

static int fetch_range_and_save(const char * ticker, const char * tf, const char * interval, const char * range) {
    char url_tk[64], safe_tk[64];
    make_url_ticker(ticker, url_tk, sizeof(url_tk));
    make_safe_ticker(ticker, safe_tk, sizeof(safe_tk));

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "curl -s -m 12 -L -A 'Mozilla/5.0' 'https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=%s&range=%s'",
        url_tk, interval, range);

    FILE * fp = popen(cmd, "r");
    if (!fp) return 0;

    char * buf = malloc(MAX_JSON_BUF);
    if (!buf) {
        pclose(fp);
        return 0;
    }

    size_t n = fread(buf, 1, MAX_JSON_BUF - 1, fp);
    pclose(fp);
    buf[n] = 0;

    if (n < 100) {
        free(buf);
        return 0;
    }

    int pts = save_tsv_series(safe_tk, tf, buf);
    free(buf);
    return pts;
}

static int fetch_stock_quote(const char * ticker, const char * conf_name, quote_data_t * q) {
    memset(q, 0, sizeof(*q));
    snprintf(q->ticker, sizeof(q->ticker), "%s", ticker);
    snprintf(q->name, sizeof(q->name), "%s", conf_name);

    char url_tk[64], safe_tk[64];
    make_url_ticker(ticker, url_tk, sizeof(url_tk));
    make_safe_ticker(ticker, safe_tk, sizeof(safe_tk));

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "curl -s -m 12 -L -A 'Mozilla/5.0' 'https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=15m&range=1d'",
        url_tk);

    FILE * fp = popen(cmd, "r");
    if (!fp) return 0;

    char * buf = malloc(MAX_JSON_BUF);
    if (!buf) {
        pclose(fp);
        return 0;
    }

    size_t n = fread(buf, 1, MAX_JSON_BUF - 1, fp);
    pclose(fp);
    buf[n] = 0;

    if (n < 100) {
        free(buf);
        return 0;
    }

    /* Extract metadata */
    if (!parse_meta_num(buf, "regularMarketPrice", &q->price)) {
        free(buf);
        return 0;
    }

    if (!parse_meta_num(buf, "previousClose", &q->prev_close)) {
        parse_meta_num(buf, "chartPreviousClose", &q->prev_close);
    }
    if (q->prev_close <= 0.0001) q->prev_close = q->price;

    q->change = q->price - q->prev_close;
    q->change_pct = (q->prev_close > 0) ? (q->change / q->prev_close) * 100.0 : 0.0;

    parse_meta_num(buf, "regularMarketDayHigh", &q->day_high);
    parse_meta_num(buf, "regularMarketDayLow", &q->day_low);
    parse_meta_num(buf, "fiftyTwoWeekHigh", &q->high_52w);
    parse_meta_num(buf, "fiftyTwoWeekLow", &q->low_52w);

    double mt = 0;
    if (parse_meta_num(buf, "regularMarketTime", &mt)) q->market_time = (long)mt;
    else q->market_time = (long)time(NULL);

    if (!parse_meta_str(buf, "currency", q->currency, sizeof(q->currency))) {
        snprintf(q->currency, sizeof(q->currency), "EUR");
    }

    char sname[64] = "";
    if (parse_meta_str(buf, "shortName", sname, sizeof(sname)) && sname[0]) {
        if (!strcmp(ticker, "^AEX")) snprintf(q->display_title, sizeof(q->display_title), "AEX");
        else snprintf(q->display_title, sizeof(q->display_title), "%.16s", sname);
    } else {
        if (!strcmp(ticker, "^AEX")) snprintf(q->display_title, sizeof(q->display_title), "AEX");
        else snprintf(q->display_title, sizeof(q->display_title), "%.16s", conf_name[0] ? conf_name : ticker);
    }

    int is_index = (ticker[0] == '^');
    format_dutch_price(q->price, is_index, q->currency, q->formatted_val, sizeof(q->formatted_val));
    format_dutch_change(q->change, q->change_pct, q->formatted_sub, sizeof(q->formatted_sub));

    /* Save 1d chart series */
    save_tsv_series(safe_tk, "1d", buf);

    free(buf);
    return 1;
}

static void persist_state(const quote_data_t * q) {
    FILE * f = fopen(STATE_PATH, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"ticker\": \"%s\",\n", q->ticker);
    fprintf(f, "  \"name\": \"%s\",\n", q->name);
    fprintf(f, "  \"title\": \"%s\",\n", q->display_title);
    fprintf(f, "  \"currency\": \"%s\",\n", q->currency);
    fprintf(f, "  \"price\": %.4f,\n", q->price);
    fprintf(f, "  \"prev_close\": %.4f,\n", q->prev_close);
    fprintf(f, "  \"change\": %.4f,\n", q->change);
    fprintf(f, "  \"change_pct\": %.4f,\n", q->change_pct);
    fprintf(f, "  \"day_high\": %.4f,\n", q->day_high);
    fprintf(f, "  \"day_low\": %.4f,\n", q->day_low);
    fprintf(f, "  \"high_52w\": %.4f,\n", q->high_52w);
    fprintf(f, "  \"low_52w\": %.4f,\n", q->low_52w);
    fprintf(f, "  \"value\": \"%s\",\n", q->formatted_val);
    fprintf(f, "  \"subtitle\": \"%s\",\n", q->formatted_sub);
    fprintf(f, "  \"market_time\": %ld,\n", q->market_time);
    fprintf(f, "  \"updated\": %ld\n", (long)time(NULL));
    fprintf(f, "}\n");
    fclose(f);
}

static void fetch_all_timeframes(const char * ticker) {
    fprintf(stderr, "[stocks_daemon] fetching all historical timeframes for %s...\n", ticker);
    fetch_range_and_save(ticker, "5d",  "15m", "5d");
    fetch_range_and_save(ticker, "1mo", "1d",  "1mo");
    fetch_range_and_save(ticker, "6mo", "1d",  "6mo");
    fetch_range_and_save(ticker, "1y",  "1d",  "1y");
}

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "[stocks_daemon] starting FreeToon Stocks Daemon...\n");
    ensure_conf_exists();
    read_conf(g_curr_ticker, sizeof(g_curr_ticker), g_curr_name, sizeof(g_curr_name));

    int elapsed_poll = POLL_INTERVAL_S;  /* fetch immediately */
    int elapsed_hist = HIST_INTERVAL_S;  /* fetch hist immediately */

    char prev_ticker[32] = "";

    while (g_running) {
        /* Check if trigger file exists or stocks.conf was touched */
        int force_reload = 0;
        struct stat st;
        if (stat(TRIG_PATH, &st) == 0) {
            unlink(TRIG_PATH);
            force_reload = 1;
        }
        if (stat(CONF_PATH, &st) == 0 && st.st_mtime != g_conf_mtime) {
            force_reload = 1;
        }

        if (force_reload) {
            read_conf(g_curr_ticker, sizeof(g_curr_ticker), g_curr_name, sizeof(g_curr_name));
            elapsed_poll = POLL_INTERVAL_S;
            if (strcmp(prev_ticker, g_curr_ticker) != 0) {
                elapsed_hist = HIST_INTERVAL_S;
            }
        }

        if (elapsed_poll >= POLL_INTERVAL_S) {
            quote_data_t q;
            fprintf(stderr, "[stocks_daemon] updating quote for %s (%s)...\n", g_curr_ticker, g_curr_name);
            if (fetch_stock_quote(g_curr_ticker, g_curr_name, &q)) {
                persist_state(&q);
                post_to_freetoon(q.display_title, q.formatted_val, q.formatted_sub);
                snprintf(prev_ticker, sizeof(prev_ticker), "%s", g_curr_ticker);
            }
            elapsed_poll = 0;
        }

        if (elapsed_hist >= HIST_INTERVAL_S) {
            fetch_all_timeframes(g_curr_ticker);
            elapsed_hist = 0;
        }

        sleep(1);
        elapsed_poll++;
        elapsed_hist++;
    }

    fprintf(stderr, "[stocks_daemon] exiting.\n");
    return 0;
}
