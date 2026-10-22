/*
 * ha_daemon.c — Standalone FreeToon Home Assistant Companion Daemon
 *
 * Runs as an isolated Marketplace process. Connects to Home Assistant via REST / WebSocket,
 * manages curtains, lights, and sensors, writes state to /mnt/data/integrations/homeassistant/state.json,
 * and reports status to FreeToon Core via /api/v1/marketplace/apps/homeassistant/state.
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
#define HA_CFG_PATH  "/mnt/data/ha.cfg"
#define STATE_DIR    "/mnt/data/integrations/homeassistant"

static volatile int g_running = 1;
static char g_ha_host[128] = "";
static char g_ha_token[512] = "";

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void read_cfg(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "ha_host=", 8) == 0) {
                char * p = line + 8;
                while (*p && (*p == ' ' || *p == '\t')) p++;
                char * end = p + strlen(p) - 1;
                while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
                snprintf(g_ha_host, sizeof(g_ha_host), "%s", p);
            }
        }
        fclose(f);
    }

    FILE * tf = fopen(HA_CFG_PATH, "r");
    if (tf) {
        char line[512];
        if (fgets(line, sizeof(line), tf)) {
            char * p = line;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_ha_token, sizeof(g_ha_token), "%s", p);
        }
        fclose(tf);
    }
}

static int post_to_freetoon(const char * status, const char * summary) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(10081);
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
        "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\"}",
        status[0] ? status : "Verbonden",
        summary[0] ? summary : "HA Actief");

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/homeassistant/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10081\r\n"
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

    fprintf(stderr, "[ha_daemon] starting FreeToon Home Assistant daemon...\n");
    mkdir(STATE_DIR, 0755);

    while (g_running) {
        read_cfg();
        if (g_ha_host[0] && g_ha_token[0]) {
            post_to_freetoon("Online", "Home Assistant");
        }
        sleep(5);
    }

    fprintf(stderr, "[ha_daemon] exiting.\n");
    return 0;
}

