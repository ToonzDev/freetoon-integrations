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
#include <strings.h>
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

#define CFG_PATH     "/mnt/data/toonui.cfg"
#define STATE_PATH   "/mnt/data/integrations/sonos/state.json"
#define MAX_ROOMS    16

typedef struct {
    char room[48];
    char ip[32];
    char uuid[48];
} daemon_room_t;

static volatile int g_running = 1;
static char g_speaker_ip[64] = "";
static char g_bootstrap_ip[64] = "";
static char g_target_zone[48] = "";
static daemon_room_t g_rooms[MAX_ROOMS];
static int g_room_count = 0;
static int g_fail_count = 0;

static char g_last_title[128] = "";
static char g_last_artist[128] = "";
static int  g_last_playing = 0;
static int  g_last_volume = -1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* XML unescaping */
static void xml_unescape_in_place(char * s) {
    if (!s) return;
    for (int pass = 0; pass < 3; pass++) {
        char * r = s;
        char * w = s;
        int changed = 0;
        while (*r) {
            if (*r == '&') {
                if (strncmp(r, "&amp;", 5) == 0) {
                    *w++ = '&'; r += 5; changed = 1;
                } else if (strncmp(r, "&quot;", 6) == 0) {
                    *w++ = '"'; r += 6; changed = 1;
                } else if (strncmp(r, "&apos;", 6) == 0) {
                    *w++ = '\''; r += 6; changed = 1;
                } else if (strncmp(r, "&#39;", 5) == 0) {
                    *w++ = '\''; r += 5; changed = 1;
                } else if (strncmp(r, "&#34;", 5) == 0) {
                    *w++ = '"'; r += 6; changed = 1;
                } else if (strncmp(r, "&lt;", 4) == 0) {
                    *w++ = '<'; r += 4; changed = 1;
                } else if (strncmp(r, "&gt;", 4) == 0) {
                    *w++ = '>'; r += 4; changed = 1;
                } else if (strncmp(r, "&nbsp;", 6) == 0) {
                    *w++ = ' '; r += 6; changed = 1;
                } else {
                    *w++ = *r++;
                }
            } else {
                *w++ = *r++;
            }
        }
        *w = '\0';
        if (!changed) break;
    }
}

/* JSON escaping */
static void json_escape(const char * src, char * dst, size_t dst_sz) {
    if (!src || !dst || dst_sz == 0) return;
    size_t o = 0;
    for (const char * p = src; *p && o + 3 < dst_sz; p++) {
        if (*p == '"') {
            dst[o++] = '\\';
            dst[o++] = '"';
        } else if (*p == '\\') {
            dst[o++] = '\\';
            dst[o++] = '\\';
        } else if ((unsigned char)*p < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
}

/* Extracts contents of <tag>...</tag> or &lt;tag...&gt;...&lt;/tag&gt; */
static int extract_xml_tag(const char * xml, const char * tag, char * out, size_t outsz) {
    if (!xml || !tag || !out || outsz == 0) return 0;
    out[0] = 0;

    char open1[64], open2[64];
    snprintf(open1, sizeof open1, "<%s", tag);
    snprintf(open2, sizeof open2, "&lt;%s", tag);
    size_t tlen = strlen(tag);

    const char * cur = xml;
    const char * p = NULL;
    int is_esc = 0;

    while (*cur) {
        const char * p1 = strstr(cur, open1);
        const char * p2 = strstr(cur, open2);
        if (!p1 && !p2) return 0;

        if (p1 && (!p2 || p1 < p2)) {
            char next = p1[1 + tlen];
            if (next == '>' || next == ' ' || next == '/' || next == '\t' || next == '\r' || next == '\n') {
                p = p1;
                is_esc = 0;
                break;
            }
            cur = p1 + 1;
        } else {
            const char * after = p2 + 4 + tlen;
            if (*after == ' ' || *after == '/' || *after == '\t' || *after == '\r' || *after == '\n' ||
                strncmp(after, "&gt;", 4) == 0) {
                p = p2;
                is_esc = 1;
                break;
            }
            cur = p2 + 1;
        }
    }
    if (!p) return 0;

    if (is_esc) {
        p = strstr(p, "&gt;");
        if (!p) return 0;
        p += 4;
    } else {
        p = strchr(p, '>');
        if (!p) return 0;
        p++;
    }

    char close1[64], close2[64];
    snprintf(close1, sizeof close1, "</%s>", tag);
    snprintf(close2, sizeof close2, "&lt;/%s&gt;", tag);

    const char * end = is_esc ? strstr(p, close2) : strstr(p, close1);
    if (!end) end = is_esc ? strstr(p, close1) : strstr(p, close2);
    if (!end) return 0;

    size_t o = 0;
    while (p < end && o + 1 < outsz) {
        out[o++] = *p++;
    }
    out[o] = 0;

    xml_unescape_in_place(out);
    return (o > 0);
}

/* Extracts attr="value" or attr=&quot;value&quot; */
static int extract_xml_attr(const char * xml, const char * attr, char * out, size_t outsz) {
    if (!xml || !attr || !out || outsz == 0) return 0;
    out[0] = 0;

    char needle1[64], needle2[64];
    snprintf(needle1, sizeof needle1, "%s=\"", attr);
    snprintf(needle2, sizeof needle2, "%s=&quot;", attr);

    const char * p = strstr(xml, needle1);
    size_t skip = strlen(needle1);
    const char * close_quote = "\"";
    if (!p) {
        p = strstr(xml, needle2);
        skip = strlen(needle2);
        close_quote = "&quot;";
    }
    if (!p) return 0;
    p += skip;

    const char * end = strstr(p, close_quote);
    if (!end) return 0;

    size_t len = (size_t)(end - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = 0;
    xml_unescape_in_place(out);
    return 1;
}

static int sonos_soap(const char * ip, const char * control_path,
                      const char * service, const char * action,
                      const char * body_args,
                      char * out_buf, size_t out_sz, int timeout_s) {
    if (!ip || !ip[0] || !control_path || !service || !action) return -1;
    if (out_buf && out_sz > 0) out_buf[0] = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv;
    tv.tv_sec = timeout_s > 0 ? timeout_s : 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(1400);
    sin.sin_addr.s_addr = inet_addr(ip);

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }

    char body[4096];
    int blen = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%s xmlns:u=\"%s\">"
        "%s"
        "</u:%s>"
        "</s:Body>"
        "</s:Envelope>",
        action, service, body_args ? body_args : "", action);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:1400\r\n"
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "Content-Length: %d\r\n"
        "SOAPACTION: \"%s#%s\"\r\n"
        "Connection: close\r\n\r\n",
        control_path, ip, blen, service, action);

    if (send(fd, header, hlen, 0) != hlen || send(fd, body, blen, 0) != blen) {
        close(fd);
        return -1;
    }

    char scratch[1024];
    char * target = (out_buf && out_sz > 0) ? out_buf : scratch;
    size_t target_sz = (out_buf && out_sz > 0) ? out_sz : sizeof(scratch);

    size_t total = 0;
    int got_200 = 0;
    while (1) {
        size_t cap = (total + 1 < target_sz) ? (target_sz - 1 - total) : (sizeof(scratch) - 1);
        char * dest = (total + 1 < target_sz) ? (target + total) : scratch;
        ssize_t r = recv(fd, dest, cap, 0);
        if (r <= 0) break;
        if (total + 1 < target_sz) {
            total += r;
            target[total] = 0;
        }
        if (!got_200 && (strstr(target, "200 OK") != NULL)) {
            got_200 = 1;
        }
    }
    close(fd);
    return got_200 ? 0 : -2;
}

static int update_topology(const char * bootstrap_ip) {
    if (!bootstrap_ip || !bootstrap_ip[0]) return 0;

    static char resp[65536];
    resp[0] = 0;

    int rc = sonos_soap(bootstrap_ip, "/ZoneGroupTopology/Control",
                        "urn:schemas-upnp-org:service:ZoneGroupTopology:1",
                        "GetZoneGroupState", "", resp, sizeof(resp), 3);
    if (rc != 0 || resp[0] == 0) {
        return 0;
    }

    g_room_count = 0;

    const char * zg = resp;
    while ((zg = strstr(zg, "Coordinator=")) != NULL && g_room_count < MAX_ROOMS) {
        char coord_uuid[48] = "";
        extract_xml_attr(zg, "Coordinator", coord_uuid, sizeof coord_uuid);
        char * c = strchr(coord_uuid, ':'); if (c) *c = 0;

        const char * zg_end = strstr(zg, "</ZoneGroup>");
        if (!zg_end) zg_end = strstr(zg, "&lt;/ZoneGroup&gt;");
        if (!zg_end) break;

        const char * mem = zg;
        while ((mem = strstr(mem, "ZoneGroupMember")) != NULL && mem < zg_end) {
            char zname[48] = "";
            char loc[128] = "";
            char uuid[48] = "";

            extract_xml_attr(mem, "ZoneName", zname, sizeof zname);
            extract_xml_attr(mem, "Location", loc, sizeof loc);
            extract_xml_attr(mem, "UUID", uuid, sizeof uuid);
            char * u = strchr(uuid, ':'); if (u) *u = 0;

            if (coord_uuid[0] && strcmp(uuid, coord_uuid) == 0 && zname[0] && loc[0]) {
                char member_ip[32] = "";
                const char * hip = strstr(loc, "http://");
                if (hip) {
                    hip += 7;
                    const char * colon = strchr(hip, ':');
                    if (colon && (size_t)(colon - hip) < sizeof member_ip) {
                        memcpy(member_ip, hip, colon - hip);
                        member_ip[colon - hip] = 0;
                    }
                }
                if (member_ip[0]) {
                    int dup = 0;
                    for (int i = 0; i < g_room_count; i++) {
                        if (strcmp(g_rooms[i].room, zname) == 0) { dup = 1; break; }
                    }
                    if (!dup) {
                        snprintf(g_rooms[g_room_count].room, sizeof g_rooms[0].room, "%s", zname);
                        snprintf(g_rooms[g_room_count].ip, sizeof g_rooms[0].ip, "%s", member_ip);
                        snprintf(g_rooms[g_room_count].uuid, sizeof g_rooms[0].uuid, "%s", uuid);
                        g_room_count++;
                    }
                }
            }
            mem += 15;
        }
        zg = zg_end;
    }

    if (g_room_count > 0) {
        snprintf(g_bootstrap_ip, sizeof(g_bootstrap_ip), "%s", bootstrap_ip);

        char matched_ip[32] = "";
        if (g_target_zone[0]) {
            for (int i = 0; i < g_room_count; i++) {
                if (strcasecmp(g_rooms[i].room, g_target_zone) == 0) {
                    snprintf(matched_ip, sizeof(matched_ip), "%s", g_rooms[i].ip);
                    break;
                }
            }
        }
        if (!matched_ip[0]) {
            snprintf(matched_ip, sizeof(matched_ip), "%s", g_rooms[0].ip);
            if (!g_target_zone[0]) {
                snprintf(g_target_zone, sizeof(g_target_zone), "%s", g_rooms[0].room);
            }
        }
        if (matched_ip[0]) {
            snprintf(g_speaker_ip, sizeof(g_speaker_ip), "%s", matched_ip);
            FILE * fip = fopen("/mnt/data/integrations/sonos/speaker.ip", "w");
            if (fip) {
                fprintf(fip, "%s\n", g_speaker_ip);
                fclose(fip);
            }
            fprintf(stderr, "[sonos_daemon] Topology resolved active speaker: %s (%s)\n",
                    g_speaker_ip, g_target_zone);
        }
    }
    return g_room_count;
}

static void read_cfg(void) {
    char old_zone[48] = "";
    snprintf(old_zone, sizeof(old_zone), "%s", g_target_zone);

    /* 1. Try saved zone from integration */
    FILE * fz = fopen("/mnt/data/integrations/sonos/zone", "r");
    if (fz) {
        char line[64] = "";
        if (fgets(line, sizeof(line), fz)) {
            char * p = line;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            if (*p) snprintf(g_target_zone, sizeof(g_target_zone), "%s", p);
        }
        fclose(fz);
    }

    /* 2. Try saved speaker.ip from integration */
    FILE * fip = fopen("/mnt/data/integrations/sonos/speaker.ip", "r");
    if (fip) {
        char line[64] = "";
        if (fgets(line, sizeof(line), fip)) {
            char * p = line;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            char * end = p + strlen(p) - 1;
            while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
            if (*p) snprintf(g_speaker_ip, sizeof(g_speaker_ip), "%s", p);
        }
        fclose(fip);
    }

    /* 3. Read toonui.cfg */
    FILE * f = fopen(CFG_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char * p = NULL;
            if (strncmp(line, "sonos_zone=", 11) == 0) {
                p = line + 11;
                while (*p && (*p == ' ' || *p == '\t')) p++;
                char * end = p + strlen(p) - 1;
                while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
                if (*p) snprintf(g_target_zone, sizeof(g_target_zone), "%s", p);
            } else if (strncmp(line, "sonos_host=", 11) == 0 || strncmp(line, "sonos_ip=", 9) == 0) {
                p = strchr(line, '=') + 1;
                while (*p && (*p == ' ' || *p == '\t')) p++;
                char * end = p + strlen(p) - 1;
                while (end > p && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
                if (*p && !g_speaker_ip[0]) snprintf(g_speaker_ip, sizeof(g_speaker_ip), "%s", p);
            }
        }
        fclose(f);
    }

    /* Switch speaker IP if target zone changed */
    if (g_target_zone[0] && g_room_count > 0 && strcasecmp(old_zone, g_target_zone) != 0) {
        for (int i = 0; i < g_room_count; i++) {
            if (strcasecmp(g_rooms[i].room, g_target_zone) == 0) {
                snprintf(g_speaker_ip, sizeof(g_speaker_ip), "%s", g_rooms[i].ip);
                fprintf(stderr, "[sonos_daemon] Zone changed to '%s' -> %s\n", g_target_zone, g_speaker_ip);
                break;
            }
        }
    }
}

static int post_to_freetoon(const char * title, const char * artist, int playing, int volume) {
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

    char esc_title[160] = "", esc_artist[160] = "";
    json_escape(title[0] ? title : (g_target_zone[0] ? g_target_zone : "Sonos"), esc_title, sizeof(esc_title));
    json_escape(artist[0] ? artist : (playing ? "Afspelen" : "Gepauzeerd"), esc_artist, sizeof(esc_artist));

    char body[512];
    int bn = snprintf(body, sizeof(body),
        "{\"value\":\"%s\",\"subtitle\":\"%s\",\"alert\":\"%s\","
        "\"playing\":%d,\"volume\":%d}",
        esc_title, esc_artist, "", playing, volume);

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "POST /api/v1/marketplace/apps/sonos/state HTTP/1.0\r\n"
        "Host: 127.0.0.1:10081\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        bn, body);

    send(s, req, rn, 0);
    close(s);

    /* Also persist locally */
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
    if (s >= 0) {
        struct sockaddr_in target = {0};
        target.sin_family = AF_INET;
        target.sin_port = htons(53);
        target.sin_addr.s_addr = inet_addr("1.1.1.1");
        if (connect(s, (struct sockaddr*)&target, sizeof(target)) == 0) {
            struct sockaddr_in local = {0};
            socklen_t len = sizeof(local);
            if (getsockname(s, (struct sockaddr*)&local, &len) == 0) {
                char ip[32];
                inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
                if (strncmp(ip, "127.", 4) != 0 && strcmp(ip, "0.0.0.0") != 0) {
                    char * last_dot = strrchr(ip, '.');
                    if (last_dot) {
                        *last_dot = 0;
                        snprintf(prefix, maxlen, "%s", ip);
                    }
                }
            }
        }
        close(s);
    }

    if (!prefix[0]) {
        FILE * fr = fopen("/proc/net/arp", "r");
        if (fr) {
            char buf[256];
            while (fgets(buf, sizeof(buf), fr)) {
                char ip[32] = "";
                if (sscanf(buf, "%31s", ip) == 1 && strchr(ip, '.')) {
                    if (strncmp(ip, "192.168.", 8) == 0 || strncmp(ip, "10.", 3) == 0 || strncmp(ip, "172.", 4) == 0) {
                        char * last_dot = strrchr(ip, '.');
                        if (last_dot) {
                            *last_dot = 0;
                            snprintf(prefix, maxlen, "%s", ip);
                            break;
                        }
                    }
                }
            }
            fclose(fr);
        }
    }
    if (!prefix[0]) snprintf(prefix, maxlen, "192.168.1");
}

#define TCP_SCAN_BATCH 64

/* Sweep subnet on port 1400 and resolve topology on responsive speakers */
static int discover_sonos_tcp(char * out_ip, size_t out_sz) {
    char prefix[32] = "";
    get_local_subnet_prefix(prefix, sizeof prefix);
    if (!prefix[0]) snprintf(prefix, sizeof prefix, "192.168.1");

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

        char candidates[8][32];
        int num_cand = 0;

        if (max_fd >= 0) {
            int sel = select(max_fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                for (int i = 0; i < count && num_cand < 8; i++) {
                    if (fds[i] >= 0 && FD_ISSET(fds[i], &wfds)) {
                        int err = 0;
                        socklen_t elen = sizeof(err);
                        if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                            snprintf(candidates[num_cand++], 32, "%s", ips[i]);
                        }
                    }
                }
            }
        }

        for (int i = 0; i < count; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }

        for (int c = 0; c < num_cand; c++) {
            if (update_topology(candidates[c]) > 0) {
                snprintf(out_ip, out_sz, "%s", g_speaker_ip);
                fprintf(stderr, "[sonos_daemon] Discovered Sonos system via %s -> active %s\n",
                        candidates[c], out_ip);
                return 0;
            }
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
                char found_ip[64] = "";
                size_t len = (size_t)(end - loc);
                memcpy(found_ip, loc, len);
                found_ip[len] = 0;
                if (update_topology(found_ip) > 0) {
                    snprintf(out_ip, maxlen, "%s", g_speaker_ip);
                    fprintf(stderr, "[sonos_daemon] SSDP resolved Sonos active speaker at %s\n", out_ip);
                    return 0;
                }
            }
        }
    }
    return -1;
}

static void poll_sonos(void) {
    if (!g_speaker_ip[0]) return;

    static char buf[16384];

    /* 1. GetTransportInfo */
    int rc = sonos_soap(g_speaker_ip, "/MediaRenderer/AVTransport/Control",
                        "urn:schemas-upnp-org:service:AVTransport:1",
                        "GetTransportInfo", "<InstanceID>0</InstanceID>",
                        buf, sizeof(buf), 2);
    if (rc == 0) {
        g_fail_count = 0;
        char state[32] = "";
        extract_xml_tag(buf, "CurrentTransportState", state, sizeof(state));
        g_last_playing = (strcmp(state, "PLAYING") == 0 || strcmp(state, "TRANSITIONING") == 0);
    } else {
        g_fail_count++;
        if (g_fail_count >= 5) {
            fprintf(stderr, "[sonos_daemon] speaker %s unreachable (5 failures), resetting IP\n", g_speaker_ip);
            g_speaker_ip[0] = 0;
            g_fail_count = 0;
        }
        return;
    }

    /* 2. GetPositionInfo */
    rc = sonos_soap(g_speaker_ip, "/MediaRenderer/AVTransport/Control",
                    "urn:schemas-upnp-org:service:AVTransport:1",
                    "GetPositionInfo", "<InstanceID>0</InstanceID>",
                    buf, sizeof(buf), 2);
    if (rc == 0) {
        char title[96] = "", artist[64] = "", stream[96] = "";
        extract_xml_tag(buf, "dc:title", title, sizeof(title));
        extract_xml_tag(buf, "dc:creator", artist, sizeof(artist));
        if (!artist[0]) extract_xml_tag(buf, "upnp:artist", artist, sizeof(artist));
        extract_xml_tag(buf, "r:streamContent", stream, sizeof(stream));

        if (stream[0]) {
            char * sep = strstr(stream, " - ");
            if (sep) {
                *sep = 0;
                snprintf(g_last_artist, sizeof(g_last_artist), "%s", stream);
                snprintf(g_last_title, sizeof(g_last_title), "%s", sep + 3);
            } else {
                snprintf(g_last_title, sizeof(g_last_title), "%s", stream);
                snprintf(g_last_artist, sizeof(g_last_artist), "%s", artist[0] ? artist : title);
            }
        } else {
            snprintf(g_last_title, sizeof(g_last_title), "%s", title[0] ? title : (g_target_zone[0] ? g_target_zone : "Sonos"));
            snprintf(g_last_artist, sizeof(g_last_artist), "%s", artist[0] ? artist : (g_last_playing ? "Afspelen" : "Gepauzeerd"));
        }
    }

    /* 3. GetVolume */
    rc = sonos_soap(g_speaker_ip, "/MediaRenderer/RenderingControl/Control",
                    "urn:schemas-upnp-org:service:RenderingControl:1",
                    "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>",
                    buf, sizeof(buf), 2);
    if (rc == 0) {
        char vol[16] = "";
        extract_xml_tag(buf, "CurrentVolume", vol, sizeof(vol));
        if (vol[0]) g_last_volume = atoi(vol);
    }

    post_to_freetoon(g_last_title, g_last_artist, g_last_playing, g_last_volume);
}

int main(int argc, char ** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "[sonos_daemon] starting FreeToon Sonos daemon...\n");
    mkdir("/mnt/data/integrations", 0755);
    mkdir("/mnt/data/integrations/sonos", 0755);

    while (g_running) {
        read_cfg();

        if (!g_speaker_ip[0] && g_bootstrap_ip[0]) {
            update_topology(g_bootstrap_ip);
        }
        if (!g_speaker_ip[0]) {
            discover_sonos_tcp(g_speaker_ip, sizeof(g_speaker_ip));
        }
        if (!g_speaker_ip[0]) {
            discover_sonos_ssdp(g_speaker_ip, sizeof(g_speaker_ip));
        }

        if (g_speaker_ip[0]) {
            poll_sonos();
        } else {
            post_to_freetoon(g_target_zone[0] ? g_target_zone : "Sonos", "Zoeken...", 0, -1);
        }

        sleep(3);
    }

    fprintf(stderr, "[sonos_daemon] exiting.\n");
    return 0;
}
