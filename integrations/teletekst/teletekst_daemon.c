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

#define STATE_PATH "/mnt/data/integrations/teletekst/state.json"
#define POLL_INTERVAL_S 300   /* 5 minutes */

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
        if (strncmp(p, "&quot;", 6) == 0) {
            *p = '"';
            memmove(p + 1, p + 6, strlen(p + 6) + 1);
        } else if (strncmp(p, "&amp;", 5) == 0) {
            *p = '&';
            memmove(p + 1, p + 5, strlen(p + 5) + 1);
        } else if (strncmp(p, "&lt;", 4) == 0) {
            *p = '<';
            memmove(p + 1, p + 4, strlen(p + 4) + 1);
        } else if (strncmp(p, "&gt;", 4) == 0) {
            *p = '>';
            memmove(p + 1, p + 4, strlen(p + 4) + 1);
        } else if (strncmp(p, "&apos;", 6) == 0 || strncmp(p, "&#39;", 5) == 0) {
            *p = '\'';
            int skip = (strncmp(p, "&#39;", 5) == 0) ? 5 : 6;
            memmove(p + 1, p + skip, strlen(p + skip) + 1);
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
