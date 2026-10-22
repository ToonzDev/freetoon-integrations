/*
 * camera_daemon.c — Standalone FreeToon Camera & Doorbell Companion Daemon
 *
 * Runs as an isolated Marketplace process. Listens on port 8765 for HTTP webhook triggers
 * (/show, /hide, /ring) from Home Assistant or smart doorbells, forwards overlay commands
 * to /tmp/toonui.cmd, and updates FreeToon Core via /api/v1/marketplace/apps/camera/state.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#define DEFAULT_PORT 8765
#define UI_SOCK      "/tmp/toonui.cmd"
#define STATE_DIR    "/mnt/data/integrations/camera"

static volatile int g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void send_ui_cmd(const char * cmd) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, UI_SOCK, sizeof(un.sun_path) - 1);
    if (connect(s, (struct sockaddr*)&un, sizeof(un)) == 0) {
        send(s, cmd, strlen(cmd), 0);
        send(s, "\n", 1, 0);
    }
    close(s);
}

static void post_to_freetoon(const char * status, const char * last_event) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        return;
    }

    char body[512];
    int bn = snprintf(body, sizeof(body),
        "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"\"}",
        status, last_event);

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/camera/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10080\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bn, body);

    send(s, req, rn, 0);
    close(s);
}

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "[camera_daemon] starting FreeToon Camera/Doorbell daemon on port %d...\n", DEFAULT_PORT);
    mkdir(STATE_DIR, 0755);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sfd);
        return 1;
    }

    if (listen(sfd, 5) != 0) {
        perror("listen");
        close(sfd);
        return 1;
    }

    post_to_freetoon("Gereed", "Geen beweging");

    while (g_running) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        char buf[1024];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            if (strstr(buf, "/show") || strstr(buf, "/ring")) {
                send_ui_cmd("camera_open");
                post_to_freetoon("Aanbellen!", "Deurbel actief");
                const char * res = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nopened\n";
                send(cfd, res, strlen(res), 0);
            } else if (strstr(buf, "/hide")) {
                send_ui_cmd("camera_close");
                post_to_freetoon("Gereed", "Deurbel gesloten");
                const char * res = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nclosed\n";
                send(cfd, res, strlen(res), 0);
            } else {
                const char * res = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nOK\n";
                send(cfd, res, strlen(res), 0);
            }
        }
        close(cfd);
    }

    close(sfd);
    fprintf(stderr, "[camera_daemon] exiting.\n");
    return 0;
}
