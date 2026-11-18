/*
 * matter_logic.c - see matter_logic.h.
 */
#define _GNU_SOURCE
#include "matter_logic.h"
#include "matter_json.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- files ---------------------------------------------------------------- */

char * mb_read_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char * buf = malloc(cap);
    while (buf) {
        if (n + 1024 + 1 > cap) {
            if (cap >= (1u << 20)) break;              /* nothing we read is this big */
            char * nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

char * mb_read_tail(const char * path, size_t max) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > (long)max) fseek(f, size - (long)max, SEEK_SET);
        else fseek(f, 0, SEEK_SET);
    }
    char * buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

int mb_mkdirs(const char * dir) {
    char tmp[256];
    if (snprintf(tmp, sizeof tmp, "%s", dir) >= (int)sizeof tmp) return -1;
    for (char * p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int mb_write_atomic(const char * path, const char * data, int do_fsync) {
    char tmp[300];
    if (snprintf(tmp, sizeof tmp, "%s.tmp-%d", path, (int)getpid()) >= (int)sizeof tmp) return -1;
    /* A fresh file only: never write through a planted file or symlink. A
     * leftover of our own (same pid after a crash) is removed once. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0 && errno == EEXIST && unlink(tmp) == 0)
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    size_t len = strlen(data), off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    if (do_fsync && fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    if (close(fd) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int mb_secure_dir(const char * dir) {
    char parent[256];
    if (snprintf(parent, sizeof parent, "%s", dir) >= (int)sizeof parent) return -1;
    char * slash = strrchr(parent, '/');
    if (slash && slash != parent) { *slash = 0; mb_mkdirs(parent); }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return -1;
    if ((st.st_mode & 077) != 0 && chmod(dir, 0700) != 0) return -1;
    return 0;
}

char * mb_read_file_nofollow(const char * path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 4096) { close(fd); return NULL; }
    char * buf = malloc((size_t)st.st_size + 1);
    ssize_t n = buf ? read(fd, buf, (size_t)st.st_size) : -1;
    close(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

static int dir_of(const char * path, char * out, size_t n) {
    if (snprintf(out, n, "%s", path) >= (int)n) return -1;
    char * slash = strrchr(out, '/');
    if (!slash || slash == out) return -1;
    *slash = 0;
    return 0;
}

static void copy(char * dst, size_t n, const char * src) {
    snprintf(dst, n, "%s", src ? src : "");
}

/* ---- processes ------------------------------------------------------------- */

int mb_pid_from_file(const char * path) {
    char * text = mb_read_file(path);
    if (!text) return 0;
    int pid = atoi(text);
    free(text);
    return pid > 1 ? pid : 0;
}

int mb_pid_alive(int pid) {
    if (pid <= 1) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

/* Start time of a process in unix seconds, 0 if unknown (/proc/<pid>/stat
 * field 22 is in clock ticks since boot; /proc/stat has the boot time). */
static time_t process_started(int pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char * st = mb_read_file(path);
    char * bt = mb_read_file("/proc/stat");
    time_t result = 0;
    if (st && bt) {
        const char * p = strrchr(st, ')');
        const char * b = strstr(bt, "\nbtime ");
        long hz = sysconf(_SC_CLK_TCK);
        if (p && b && hz > 0) {
            p++;
            /* tokens after ')': state is field 3, starttime field 22 */
            for (int field = 3; field <= 22 && *p; field++) {
                while (*p == ' ') p++;
                if (field == 22) {
                    unsigned long long ticks = strtoull(p, NULL, 10);
                    result = (time_t)(atoll(b + 7) + (long long)(ticks / (unsigned long long)hz));
                    break;
                }
                while (*p && *p != ' ') p++;
            }
        }
    }
    free(st);
    free(bt);
    return result;
}

/* ---- paths ----------------------------------------------------------------- */

void mb_default_paths(mb_paths_t * p) {
    p->status = MB_STATUS_PATH;
    p->prefs = MB_PREFS_PATH;
    p->comfort = MB_COMFORT_PATH;
    p->pidfile = MB_PID_PATH;
    p->service_log = MB_SERVICE_LOG;
    p->action = MB_ACTION_PATH;
    p->script = NULL;
}

const char * mb_action_name(mb_action_t a) {
    switch (a) {
    case MB_ACT_START: return "start";
    case MB_ACT_RESTART: return "restart";
    case MB_ACT_RESET: return "reset";
    default: return "idle";
    }
}

mb_action_t mb_action_from_name(const char * s) {
    if (!s) return MB_ACT_IDLE;
    if (!strcmp(s, "start")) return MB_ACT_START;
    if (!strcmp(s, "restart")) return MB_ACT_RESTART;
    if (!strcmp(s, "reset")) return MB_ACT_RESET;
    return MB_ACT_IDLE;
}

void mb_format_code(const char * code, char * out, size_t n) {
    size_t len = code ? strlen(code) : 0;
    if (len == 11) snprintf(out, n, "%.4s-%.3s-%.4s", code, code + 4, code + 7);
    else snprintf(out, n, "%s", code ? code : "");
}

/* ---- snapshot -------------------------------------------------------------- */

/* Last error line of the service log, without its "[hh:mm:ss.mmm] " stamp. */
static void last_log_error(const char * path, char * out, size_t n) {
    out[0] = 0;
    char * tail = mb_read_tail(path, 8192);
    if (!tail) return;
    char * line = tail, * found = NULL;
    while (line && *line) {
        char * nl = strchr(line, '\n');
        if (nl) *nl = 0;
        /* Matterbridge logs a harmless "npm root -g" failure (no npm on the
         * Toon) with a stack trace on every start: not the error to show. */
        if ((strstr(line, "[error]") || strstr(line, "[fatal]") || strstr(line, "Error:")
             || strstr(line, "mislukt")) && !strstr(line, "npm") && !strstr(line, "node_modules"))
            found = line;
        line = nl ? nl + 1 : NULL;
    }
    if (found) {
        /* drop the "[time] [source] [level] " groups in front of the text */
        while (found[0] == '[') { char * e = strchr(found, ']'); if (!e || e[1] != ' ') break; found = e + 2; }
        snprintf(out, n, "%s", found);
        for (char * c = out; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    }
    free(tail);
}

static int prefs_selected(const mj_t * prefs, const char * id) {
    return mj_is_true(mj_get(mj_get(prefs, "selected_devices"), id));
}

static mb_device_t * add_device(mb_snapshot_t * s, const char * id, const char * name, mb_kind_t kind) {
    if (s->n_devices >= MB_MAX_DEVICES || !id || !id[0]) return NULL;
    for (int i = 0; i < s->n_devices; i++) if (!strcmp(s->devices[i].id, id)) return NULL;
    mb_device_t * d = &s->devices[s->n_devices++];
    memset(d, 0, sizeof *d);
    copy(d->id, sizeof d->id, id);
    copy(d->name, sizeof d->name, name);
    d->kind = kind;
    d->online = 1;
    return d;
}

void mb_snapshot_load(mb_snapshot_t * s, const mb_paths_t * p, int en) {
    memset(s, 0, sizeof *s);

    /* service script and process */
    const char * candidates[2] = { p->script ? p->script : MB_SERVICE_SCRIPT, p->script ? NULL : MB_SERVICE_SCRIPT2 };
    for (int i = 0; i < 2 && !s->script[0]; i++)
        if (candidates[i] && access(candidates[i], X_OK) == 0) copy(s->script, sizeof s->script, candidates[i]);
    int pid = mb_pid_from_file(p->pidfile);
    int alive = mb_pid_alive(pid);
    s->pid = alive ? pid : 0;

    /* status.json */
    struct stat st_status;
    int have_status = stat(p->status, &st_status) == 0;
    char * text = have_status ? mb_read_file(p->status) : NULL;
    mj_t * status = mj_parse(text);
    free(text);
    if (status) {
        s->status_ok = 1;
        s->status_running = mj_is_true(mj_get(status, "running"));
        s->commissioned = mj_is_true(mj_get(status, "commissioned"));
        copy(s->message, sizeof s->message, mj_str(mj_get(status, "message"), ""));
        /* bridge 0.2.1 says "gekoppeld met Google Home" for any controller */
        char * gh = strstr(s->message, "; gekoppeld met Google Home");
        if (gh) *gh = 0;
        copy(s->pairing_code, sizeof s->pairing_code, mj_str(mj_get(status, "pairing_code"), ""));
        copy(s->qr_payload, sizeof s->qr_payload, mj_str(mj_get(status, "qr_payload"), ""));
        const mj_t * energy = mj_get(status, "energy");
        s->energy_available = mj_is_true(mj_get(energy, "available"));
        s->energy_online = mj_is_true(mj_get(energy, "online"));
        s->import_w = mj_num(mj_get(energy, "import_power_w"), 0);
        s->export_w = mj_num(mj_get(energy, "export_power_w"), 0);
        s->net_w = mj_num(mj_get(energy, "net_power_w"), 0);
        s->gas_lph = mj_num(mj_get(energy, "gas_flow_lph"), 0);
    }

    if (!s->script[0] && !alive) s->service = MB_SVC_MISSING;
    else if (!alive) s->service = MB_SVC_STOPPED;
    else {
        /* The bridge writes status.json on its first sync (about a minute
         * after start) and when it stops; a file older than the process is
         * from the previous run. */
        time_t started = process_started(pid);
        int fresh = have_status && (started == 0 || st_status.st_mtime + 2 >= started);
        s->service = s->status_ok && s->status_running && fresh ? MB_SVC_RUNNING : MB_SVC_STARTING;
    }
    if (s->service != MB_SVC_RUNNING) {
        /* a stale file must not offer a QR code for a bridge that is not listening */
        s->pairing_code[0] = s->qr_payload[0] = 0;
        if (s->service != MB_SVC_STARTING) last_log_error(p->service_log, s->log_error, sizeof s->log_error);
    }

    /* devices: the Toon thermostat, ComfortHub climate devices, the energy meter */
    text = mb_read_file(p->prefs);
    mj_t * prefs = mj_parse(text);
    free(text);

    mb_device_t * d = add_device(s, MB_THERMOSTAT_ID, en ? "Toon thermostat" : "Toon thermostaat", MB_KIND_THERMOSTAT);
    if (d) copy(d->detail, sizeof d->detail, en ? "Toon room thermostat, Matter thermostat"
                                                 : "Toon-kamerthermostaat, Matter-thermostaat");

    text = mb_read_file(p->comfort);
    mj_t * comfort = mj_parse(text);
    free(text);
    const mj_t * state = mj_get(comfort, "state");
    if (!state) state = comfort;
    const mj_t * list = mj_get(state, "devices");
    for (const mj_t * c = list && list->type == MJ_ARR ? list->child : NULL; c; c = c->next) {
        const char * type = mj_str(mj_get(c, "matter_type"), "");
        /* the bridge plugin only exposes thermostats from this list */
        if (!mj_is_true(mj_get(c, "matter_eligible")) || strcmp(type, "thermostat") != 0) continue;
        char id[64];
        const mj_t * idn = mj_get(c, "id");
        if (idn && idn->type == MJ_NUM) snprintf(id, sizeof id, "%.0f", idn->num);
        else copy(id, sizeof id, mj_str(idn, ""));
        const char * name = mj_str(mj_get(c, "name"), "");
        if (!name[0]) continue;
        d = add_device(s, id, name, MB_KIND_CLIMATE);
        if (!d) continue;
        d->online = mj_is_true(mj_get(c, "online"));
        const char * brand = mj_str(mj_get(c, "brand"), mj_str(mj_get(c, "provider"), en ? "Climate" : "Klimaat"));
        const char * tech = mj_str(mj_get(c, "technology"), "");
        snprintf(d->detail, sizeof d->detail, "%s%s%s, %s", brand, tech[0] ? " " : "", tech,
                 en ? "Matter thermostat" : "Matter-thermostaat");
    }
    mj_free(comfort);

    if (s->energy_available) {
        d = add_device(s, MB_ENERGY_ID, en ? "Toon energy meter" : "Toon energiemeter", MB_KIND_ENERGY);
        if (d) {
            d->online = s->energy_online;
            long w = lround(fabs(s->net_w));
            const char * dir = s->net_w < 0 ? (en ? "export" : "teruglevering") : (en ? "import" : "afname");
            if (s->gas_lph > 0)
                snprintf(d->detail, sizeof d->detail, en ? "Electricity %ld W %s, gas %ld l/h"
                                                         : "Elektriciteit %ld W %s, gas %ld l/u",
                         w, dir, lround(s->gas_lph));
            else
                snprintf(d->detail, sizeof d->detail, en ? "Electricity %ld W %s" : "Elektriciteit %ld W %s", w, dir);
        }
    }

    for (int i = 0; i < s->n_devices; i++) {
        s->devices[i].selected = prefs_selected(prefs, s->devices[i].id);
        s->n_selected += s->devices[i].selected;
    }
    mj_free(prefs);

    /* daemon state */
    text = mb_read_file(p->action);
    mj_t * act = mj_parse(text);
    free(text);
    if (act) {
        s->daemon_alive = mb_pid_alive((int)mj_num(mj_get(act, "daemon_pid"), 0));
        s->action = s->daemon_alive ? mb_action_from_name(mj_str(mj_get(act, "action"), "")) : MB_ACT_IDLE;
        s->last_action = mb_action_from_name(mj_str(mj_get(act, "last_action"), ""));
        s->last_failed = mj_is_true(mj_get(act, "last_failed"));
        s->last_done = (long long)mj_num(mj_get(act, "last_done"), 0);
        copy(s->last_message, sizeof s->last_message, mj_str(mj_get(act, "last_message"), ""));
        s->autostart_paused = mj_is_true(mj_get(act, "autostart_paused"));
        mj_free(act);
    }
    mj_free(status);
}

/* ---- writing --------------------------------------------------------------- */

typedef struct { char id[64]; int on; } sel_t;

int mb_set_selected(const char * prefs_path, const char * id, int on) {
    if (!id || !id[0]) return -1;
    sel_t sel[64];
    int n = 0, found = 0;

    char * text = mb_read_file(prefs_path);
    mj_t * prefs = mj_parse(text);
    free(text);
    const mj_t * map = mj_get(prefs, "selected_devices");
    for (const mj_t * c = map && map->type == MJ_OBJ ? map->child : NULL; c && n < 64; c = c->next) {
        if (!c->key || c->type != MJ_BOOL) continue;
        copy(sel[n].id, sizeof sel[n].id, c->key);
        sel[n].on = c->boolean;
        if (!strcmp(c->key, id)) { sel[n].on = on ? 1 : 0; found = 1; }
        n++;
    }
    mj_free(prefs);
    if (!found) {
        if (n >= 64) return -1;
        copy(sel[n].id, sizeof sel[n].id, id);
        sel[n].on = on ? 1 : 0;
        n++;
    }

    /* the layout JSON.stringify(..., null, 2) of the QML app */
    size_t cap = 256 + (size_t)n * 160;
    char * out = malloc(cap);
    if (!out) return -1;
    size_t len = (size_t)snprintf(out, cap, "{\n  \"schema\": \"%s\",\n  \"selected_devices\": {", MB_PREFS_SCHEMA);
    for (int i = 0; i < n; i++) {
        char key[160];
        mj_quote(sel[i].id, key, sizeof key);
        len += (size_t)snprintf(out + len, cap - len, "%s\n    %s: %s", i ? "," : "", key, sel[i].on ? "true" : "false");
    }
    snprintf(out + len, cap - len, "%s}\n}\n", n ? "\n  " : "");

    char dir[256];
    if (dir_of(prefs_path, dir, sizeof dir) == 0) mb_mkdirs(dir);
    int rc = mb_write_atomic(prefs_path, out, 1);
    free(out);
    return rc;
}

int mb_send_command(const char * path, const char * command) {
    char dir[256], line[64];
    if (dir_of(path, dir, sizeof dir) != 0 || mb_secure_dir(dir) != 0) return -1;
    snprintf(line, sizeof line, "%s\n", command);
    return mb_write_atomic(path, line, 0);
}
