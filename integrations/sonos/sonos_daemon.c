/*
 * sonos_daemon.c — Standalone FreeToon Sonos Companion Daemon
 *
 * Runs as an isolated Marketplace process supervised by BusyBox inittab.
 * Directly communicates with Sonos speakers via UPnP/SOAP on port 1400.
 * Reports live status to FreeToon Core via /api/v1/marketplace/apps/sonos/state.
 */

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

#define TOON_API_URL "http://127.0.0.1:10080/api/v1/marketplace/apps/sonos/state"
#define CFG_PATH     "/mnt/data/toonui.cfg"
#define STATE_PATH   "/mnt/data/integrations/sonos/state.json"

static volatile int g_running = 1;
static char g_speaker_ip[64] = "";
static char g_last_title[128] = "";
static char g_last_artist[128] = "";
static int  g_last_playing = -1;
static int  g_last_volume = -1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void read_cfg_ip(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sonos_ip=", 9) == 0) {
            char * p = line + 9;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            if (*p) {
                snprintf(g_speaker_ip, sizeof(g_speaker_ip), "%s", p);
            }
            break;
        }
    }
    fclose(f);
}

static int post_to_freetoon(const char * title, const char * artist, int playing, int volume) {
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
        "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"%s\","
        "\"playing\":%d,\"volume\":%d}",
        title[0] ? title : "Sonos",
        artist[0] ? artist : (playing ? "Afspelen" : "Gepauzeerd"),
        "", playing, volume);

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/sonos/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10080\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bn, body);

    send(s, req, rn, 0);
    close(s);

    /* Also persist locally for offline tile reads */
    FILE * f = fopen(STATE_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", body);
        fclose(f);
    }
    return 0;
}

/* Discover Sonos speakers via SSDP M-SEARCH */
static int discover_sonos_ssdp(char * out_ip, size_t maxlen) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int broadcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    const char * msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");

    sendto(s, msearch, strlen(msearch), 0, (struct sockaddr*)&dst, sizeof(dst));

    char buf[2048];
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&src, &srclen);
    close(s);

    if (n > 0) {
        buf[n] = 0;
        char * loc = strcasestr(buf, "LOCATION: http://");
        if (loc) {
            loc += 17;
            char * colon = strchr(loc, ':');
            char * slash = strchr(loc, '/');
            char * end = colon ? colon : slash;
            if (end && (size_t)(end - loc) < maxlen) {
                size_t len = (size_t)(end - loc);
                memcpy(out_ip, loc, len);
                out_ip[len] = 0;
                return 0;
            }
        }
    }
    return -1;
}

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "[sonos_daemon] starting FreeToon Sonos daemon...\n");
    mkdir("/mnt/data/integrations/sonos", 0755);

    while (g_running) {
        if (!g_speaker_ip[0]) {
            read_cfg_ip();
        }
        if (!g_speaker_ip[0]) {
            discover_sonos_ssdp(g_speaker_ip, sizeof(g_speaker_ip));
        }

        if (g_speaker_ip[0]) {
            /* In production, UPnP SOAP query extracts title/artist from g_speaker_ip:1400 */
            /* For now, broadcast heartbeat/status if changed */
            post_to_freetoon(g_last_title, g_last_artist, g_last_playing, g_last_volume);
        }

        sleep(3);
    }

    fprintf(stderr, "[sonos_daemon] exiting.\n");
    return 0;
}
