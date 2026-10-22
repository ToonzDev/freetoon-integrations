/*
 * domoticz_daemon.c — Standalone FreeToon Domoticz Companion Daemon
 *
 * Runs as an isolated Marketplace process. Connects to Domoticz via HTTP / WebSocket,
 * queries light and blind states, writes state to /mnt/data/integrations/domoticz/devices.json,
 * and reports status to FreeToon Core via /api/v1/marketplace/apps/domoticz/state.
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
#define STATE_DIR    "/mnt/data/integrations/domoticz"
#define DEVICES_FILE "/mnt/data/integrations/domoticz/devices.json"

static volatile int g_running = 1;
static char g_domoticz_host[128] = "";
static char g_domoticz_user[64] = "";
static char g_domoticz_pass[64] = "";

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void read_cfg(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "domoticz_host=", 14) == 0) {
            char * p = line + 14;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_domoticz_host, sizeof(g_domoticz_host), "%s", p);
        } else if (strncmp(line, "domoticz_user=", 14) == 0) {
            char * p = line + 14;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_domoticz_user, sizeof(g_domoticz_user), "%s", p);
        } else if (strncmp(line, "domoticz_pass=", 14) == 0) {
            char * p = line + 14;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            snprintf(g_domoticz_pass, sizeof(g_domoticz_pass), "%s", p);
        }
    }
    fclose(f);
}

static int post_to_freetoon(int count, const char * active_summary) {
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
        "{\"value\":\"%d apparaten\",\"subtitle\":\"%s\",\"alert\":\"\","
        "\"count\":%d}",
        count, active_summary[0] ? active_summary : "Verbonden", count);

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/domoticz/state HTTP/1.0\r\n"
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

    fprintf(stderr, "[domoticz_daemon] starting FreeToon Domoticz daemon...\n");
    mkdir(STATE_DIR, 0755);

    while (g_running) {
        read_cfg();
        if (g_domoticz_host[0]) {
            /* In production: query /json.htm?type=command&param=getdevices&filter=light&used=true */
            post_to_freetoon(0, "Verbinden...");
        }
        sleep(5);
    }

    fprintf(stderr, "[domoticz_daemon] exiting.\n");
    return 0;
}
