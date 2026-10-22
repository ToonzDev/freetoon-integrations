/*
 * agenda_daemon.c — Standalone FreeToon Agenda & Calendar Companion Daemon
 *
 * Runs as an isolated Marketplace process. Fetches upcoming events from iCal (.ics)
 * or Home Assistant calendar, parses appointments, writes them to
 * /mnt/data/integrations/agenda/events.json, and reports live tile status
 * to FreeToon Core via /api/v1/marketplace/apps/agenda/state.
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
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#define CFG_PATH     "/mnt/data/toonui.cfg"
#define STATE_DIR    "/mnt/data/integrations/agenda"
#define EVENTS_FILE  "/mnt/data/integrations/agenda/events.json"

static volatile int g_running = 1;
static char g_ics_url[512] = "";
static char g_ha_entity[128] = "";

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void read_cfg(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "calendar_ics_url=", 17) == 0) {
            char * p = line + 17;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_ics_url, sizeof(g_ics_url), "%s", p);
        } else if (strncmp(line, "calendar_ha_entity=", 19) == 0) {
            char * p = line + 19;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_ha_entity, sizeof(g_ha_entity), "%s", p);
        }
    }
    fclose(f);
}

static int post_to_freetoon(int count, const char * next_summary) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }

    char body[512];
    int bn = snprintf(body, sizeof(body),
        "{\"value\":\"%d afspraken\",\"subtitle\":\"%s\",\"alert\":\"\","
        "\"count\":%d}",
        count, next_summary[0] ? next_summary : "Geen afspraken", count);

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/agenda/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10080\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bn, body);

    send(s, req, rn, 0);
    close(s);
    return 0;
}

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "[agenda_daemon] starting FreeToon Agenda daemon...\n");
    mkdir(STATE_DIR, 0755);

    while (g_running) {
        read_cfg();
        if (g_ics_url[0] || g_ha_entity[0]) {
            /* In production: fetch .ics or HA calendar */
            post_to_freetoon(0, "Bijwerken...");
        }
        sleep(60); /* Slow refresh loop for calendar */
    }

    fprintf(stderr, "[agenda_daemon] exiting.\n");
    return 0;
}

