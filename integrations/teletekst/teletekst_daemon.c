/*
 * teletekst_daemon.c — FreeToon NOS Teletekst Companion Daemon
 *
 * Runs in background. Periodically fetches NOS Teletekst frontpage (page 101)
 * and updates the homescreen tile with the latest breaking news headline.
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
#include <stdint.h>

#define STATE_PATH "/mnt/data/integrations/teletekst/state.json"
#define POLL_INTERVAL_S 300   /* 5 minutes */

typedef struct {
    const char * name;
    uint32_t cp;
} html_entity_t;

static const html_entity_t s_html_entities[] = {
    { "&quot;",   0x0022 },
    { "&amp;",    0x0026 },
    { "&apos;",   0x0027 },
    { "&#39;",    0x0027 },
    { "&lt;",     0x003C },
    { "&gt;",     0x003E },
    { "&nbsp;",   0x0020 },
    { "&euml;",   0x00EB },
    { "&eacute;", 0x00E9 },
    { "&egrave;", 0x00E8 },
    { "&iuml;",   0x00EF },
    { "&oacute;", 0x00F3 },
    { "&ouml;",   0x00F6 },
    { "&auml;",   0x00E4 },
    { "&uuml;",   0x00FC },
    { "&ccedil;", 0x00E7 },
    { "&Euml;",   0x00CB },
    { "&Eacute;", 0x00C9 },
    { "&Egrave;", 0x00C8 },
    { "&Iuml;",   0x00CF },
    { "&Oacute;", 0x00D3 },
    { "&Ouml;",   0x00D6 },
    { "&Auml;",   0x00C4 },
    { "&Uuml;",   0x00DC },
    { "&Ccedil;", 0x00C7 },
    { "&aacute;", 0x00E1 },
    { "&iacute;", 0x00ED },
    { "&uacute;", 0x00FA },
    { "&agrave;", 0x00E0 },
    { "&igrave;", 0x00EC },
    { "&ograve;", 0x00F2 },
    { "&ugrave;", 0x00F9 },
    { "&acirc;",  0x00E2 },
    { "&ecirc;",  0x00EA },
    { "&icirc;",  0x00EE },
    { "&ocirc;",  0x00F4 },
    { "&ucirc;",  0x00FB },
    { "&atilde;", 0x00E3 },
    { "&otilde;", 0x00F5 },
    { "&ntilde;", 0x00F1 },
    { "&Ntilde;", 0x00D1 },
    { "&szlig;",  0x00DF },
    { "&euro;",   0x20AC },
    { "&copy;",   0x00A9 },
    { "&reg;",    0x00AE },
    { "&deg;",    0x00B0 },
    { "&plusmn;", 0x00B1 },
    { "&sup2;",   0x00B2 },
    { "&sup3;",   0x00B3 },
    { "&micro;",  0x00B5 },
    { "&middot;", 0x00B7 },
    { "&frac12;", 0x00BD },
    { "&frac14;", 0x00BC },
    { "&frac34;", 0x00BE },
    { "&iquest;", 0x00BF },
    { "&iexcl;",  0x00A1 },
    { NULL, 0 }
};

static inline size_t encode_utf8(uint32_t cp, char * out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static int decode_html_entity(const char * p, uint32_t * out_cp, int * entity_len) {
    if (*p != '&') return 0;
    if (strncmp(p, "&#x", 3) == 0 || strncmp(p, "&#X", 3) == 0) {
        const char * semi = strchr(p + 3, ';');
        if (semi && semi <= p + 10) {
            char hex_buf[16];
            size_t len = (size_t)(semi - (p + 3));
            memcpy(hex_buf, p + 3, len);
            hex_buf[len] = 0;
            char * end = NULL;
            unsigned long val = strtoul(hex_buf, &end, 16);
            if (end == hex_buf + len) {
                *out_cp = (uint32_t)val;
                *entity_len = (int)(semi - p + 1);
                return 1;
            }
        }
    }
    if (strncmp(p, "&#", 2) == 0 && isdigit((unsigned char)p[2])) {
        const char * semi = strchr(p + 2, ';');
        if (semi && semi <= p + 10) {
            char dec_buf[16];
            size_t len = (size_t)(semi - (p + 2));
            memcpy(dec_buf, p + 2, len);
            dec_buf[len] = 0;
            char * end = NULL;
            unsigned long val = strtoul(dec_buf, &end, 10);
            if (end == dec_buf + len) {
                *out_cp = (uint32_t)val;
                *entity_len = (int)(semi - p + 1);
                return 1;
            }
        }
    }
    for (int i = 0; s_html_entities[i].name != NULL; i++) {
        size_t nlen = strlen(s_html_entities[i].name);
        if (strncmp(p, s_html_entities[i].name, nlen) == 0) {
            *out_cp = s_html_entities[i].cp;
            *entity_len = (int)nlen;
            return 1;
        }
    }
    return 0;
}

static volatile int g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int post_to_freetoon(const char * title, const char * page) {
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
        "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\"}",
        title && title[0] ? title : "NOS Teletekst",
        page && page[0] ? page : "Pagina 101");

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/teletekst/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10081\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bn, body);

    send(s, req, rn, 0);
    close(s);

    /* Also persist locally for fast reboot load */
    mkdir("/mnt/data/integrations/teletekst", 0755);
    FILE * f = fopen(STATE_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", body);
        fclose(f);
    }
    return 0;
}

static void clean_html_escapes(char * s) {
    char * p = s;
    while (*p) {
        uint32_t cp = 0;
        int elen = 0;
        if (decode_html_entity(p, &cp, &elen)) {
            char u[8];
            size_t un = encode_utf8(cp, u);
            memmove(p + un, p + elen, strlen(p + elen) + 1);
            memcpy(p, u, un);
            p += un;
            continue;
        }
        p++;
    }
}

static void update_headline(void) {
    FILE * fp = popen("/usr/bin/curl -s -4 -k --max-time 6 --connect-timeout 4 https://teletekst-data.nos.nl/json/101", "r");
    if (!fp) return;

    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    buf[n] = 0;

    if (n < 100) return;

    /* Find content string */
    char * c_start = strstr(buf, "\"content\":\"");
    if (!c_start) return;
    c_start += 11;

    /* In page 101, headlines are formatted in <span class=\"cyan \"> <text></span>...#<page> */
    char headline[128] = "";
    char target_page[32] = "NOS Teletekst 101";

    char * span = strstr(c_start, "<span class=\\\"cyan \\\">");
    if (!span) span = strstr(c_start, "class=\\\"cyan");
    if (span) {
        char * text_start = strchr(span, '>');
        if (text_start) {
            text_start++;
            while (*text_start == ' ' || *text_start == '\t') text_start++;
            char * text_end = strstr(text_start, "<");
            if (text_end) {
                size_t hlen = (size_t)(text_end - text_start);
                if (hlen >= sizeof(headline)) hlen = sizeof(headline) - 1;
                memcpy(headline, text_start, hlen);
                headline[hlen] = 0;

                /* Trim trailing dots/spaces */
                while (hlen > 0 && (headline[hlen - 1] == '.' || headline[hlen - 1] == ' ')) {
                    headline[--hlen] = 0;
                }
                clean_html_escapes(headline);

                /* Look for page number link right after */
                char * a_tag = strstr(text_end, "href=\\\"#");
                if (a_tag && a_tag < text_end + 120) {
                    a_tag += 8;
                    char pnum[16] = "";
                    size_t pi = 0;
                    while (*a_tag >= '0' && *a_tag <= '9' && pi < sizeof(pnum) - 1) {
                        pnum[pi++] = *a_tag++;
                    }
                    pnum[pi] = 0;
                    if (pnum[0]) {
                        snprintf(target_page, sizeof(target_page), "Teletekst %s", pnum);
                    }
                }
            }
        }
    }

    if (headline[0]) {
        post_to_freetoon(headline, target_page);
    }
}

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "[teletekst_daemon] starting NOS Teletekst background daemon...\n");
    mkdir("/mnt/data/integrations/teletekst", 0755);

    int elapsed = POLL_INTERVAL_S; /* update immediately on startup */

    while (g_running) {
        if (elapsed >= POLL_INTERVAL_S) {
            update_headline();
            elapsed = 0;
        }
        sleep(1);
        elapsed++;
    }

    fprintf(stderr, "[teletekst_daemon] exiting.\n");
    return 0;
}
