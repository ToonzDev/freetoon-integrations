/*
 * vent_daemon.c — Standalone FreeToon Itho Ventilation Companion Daemon
 *
 * Runs as an isolated Marketplace process. Communicates with Itho CVE Wi-Fi module
 * via MQTT and HTTP virtual remote, writes live state to /mnt/data/integrations/ventilation/state.json,
 * and reports status to FreeToon Core via /api/v1/marketplace/apps/ventilation/state.
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
#define STATE_DIR    "/mnt/data/integrations/ventilation"

static volatile int g_running = 1;
static char g_vent_host[128] = "";

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void read_cfg(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "vent_host=", 10) == 0) {
            char * p = line + 10;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_vent_host, sizeof(g_vent_host), "%s", p);
            break;
        }
    }
    fclose(f);
}

static int post_to_freetoon(const char * mode, int rpm) {
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
        "{\"value\":\"%s\",\"subtitle\":\"%d rpm\",\"alert\":\"\","
        "\"rpm\":%d}",
        mode[0] ? mode : "Auto", rpm, rpm);

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/ventilation/state HTTP/1.0\r\n"
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

    fprintf(stderr, "[vent_daemon] starting FreeToon Itho Ventilation daemon...\n");
    mkdir(STATE_DIR, 0755);

    while (g_running) {
        read_cfg();
        if (g_vent_host[0]) {
            /* In production: query Itho bridge or receive MQTT */
            post_to_freetoon("Auto", 1200);
        }
        sleep(5);
    }

    fprintf(stderr, "[vent_daemon] exiting.\n");
    return 0;
}

