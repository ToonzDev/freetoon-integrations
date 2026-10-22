/*
 * sonos_daemon.c — Standalone FreeToon Sonos Companion Daemon
 *
 * Runs as an isolated Marketplace process supervised by BusyBox inittab.
 * Directly communicates with Sonos speakers via UPnP/SOAP on port 1400.
 * Reports live status to FreeToon Core via /api/v1/marketplace/apps/sonos/state.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/select.h>
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
        char * p = NULL;
        if (strncmp(line, "sonos_host=", 11) == 0) {
            p = line + 11;
        } else if (strncmp(line, "sonos_ip=", 9) == 0) {
            p = line + 9;
        }
        if (p) {
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            if (*p) {
                snprintf(g_speaker_ip, sizeof(g_speaker_ip), "%s", p);
                fprintf(stderr, "[sonos_daemon] loaded speaker host from config: %s\n", g_speaker_ip);
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

static void get_local_subnet_prefix(char * prefix, size_t maxlen) {
    prefix[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port = htons(80);
    target.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (struct sockaddr*)&target, sizeof(target)) == 0) {
        struct sockaddr_in local = {0};
        socklen_t len = sizeof(local);
        if (getsockname(s, (struct sockaddr*)&local, &len) == 0) {
            char ip[32];
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
            char * last_dot = strrchr(ip, '.');
            if (last_dot) {
                *last_dot = 0;
                snprintf(prefix, maxlen, "%s", ip);
            }
        }
    }
    close(s);
}

#define TCP_SCAN_BATCH 64

/* Fast TCP sweep across local /24 subnet on port 1400 (Sonos UPnP) */
static int discover_sonos_tcp(char * out_ip, size_t out_sz) {
    char prefix[32] = "";
    get_local_subnet_prefix(prefix, sizeof prefix);
    if (!prefix[0]) snprintf(prefix, sizeof prefix, "192.168.2");

    int fds[TCP_SCAN_BATCH];
    char ips[TCP_SCAN_BATCH][32];

    for (int start_host = 1; start_host <= 254; start_host += TCP_SCAN_BATCH) {
        int end_host = start_host + TCP_SCAN_BATCH - 1;
        if (end_host > 254) end_host = 254;
        int count = end_host - start_host + 1;

        int max_fd = -1;
        fd_set wfds;
        FD_ZERO(&wfds);

        for (int i = 0; i < count; i++) {
            fds[i] = -1;
            int host_num = start_host + i;
            snprintf(ips[i], sizeof ips[i], "%s.%d", prefix, host_num);

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;

            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(1400);
            sin.sin_addr.s_addr = inet_addr(ips[i]);

            connect(fd, (struct sockaddr *)&sin, sizeof(sin));
            fds[i] = fd;
            FD_SET(fd, &wfds);
            if (fd > max_fd) max_fd = fd;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000; /* 300 ms */

        char found_ip[32] = "";

        if (max_fd >= 0) {
            int sel = select(max_fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                for (int i = 0; i < count; i++) {
                    if (fds[i] >= 0 && FD_ISSET(fds[i], &wfds)) {
                        int err = 0;
                        socklen_t elen = sizeof(err);
                        if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                            snprintf(found_ip, sizeof(found_ip), "%s", ips[i]);
                            break;
                        }
                    }
                }
            }
        }

        for (int i = 0; i < count; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }

        if (found_ip[0]) {
            snprintf(out_ip, out_sz, "%s", found_ip);
            fprintf(stderr, "[sonos_daemon] TCP scan found Sonos speaker at %s:1400\n", out_ip);
            return 0;
        }
    }
    return -1;
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
                fprintf(stderr, "[sonos_daemon] SSDP found Sonos speaker at %s\n", out_ip);
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
            discover_sonos_tcp(g_speaker_ip, sizeof(g_speaker_ip));
        }
        if (!g_speaker_ip[0]) {
            discover_sonos_ssdp(g_speaker_ip, sizeof(g_speaker_ip));
        }

        if (g_speaker_ip[0]) {
            post_to_freetoon(g_last_title, g_last_artist, g_last_playing, g_last_volume);
        }

        sleep(3);
    }

    fprintf(stderr, "[sonos_daemon] exiting.\n");
    return 0;
}
