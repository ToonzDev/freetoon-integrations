/*
 * matter_logic.h - shared by the Matter daemon and its UI plugin.
 *
 * The Matter Bridge itself is the Node service of the Matter Bridge ToonStore
 * app (ToonSoftwareCollective/matterbridge), installed in
 * /mnt/data/tsc/matter-service and controlled with toon-matter-service.sh.
 * This integration does not reimplement it; it reads the files the bridge
 * already writes and writes the device choice the bridge already reads:
 *
 *   status   /var/volatile/tmp/toon-matter-bridge/status.json
 *            (nl.toon.matter-bridge/status-v1, written by the bridge plugin)
 *   choice   /mnt/data/tsc/matter-bridge.json
 *            (nl.toon.matter-bridge/preferences-v1, same as the QML app;
 *            the bridge picks it up within 5 s, no restart)
 *   devices  /var/volatile/tmp/toon-app-bus/v1/comfort-hub/state.json
 *            (optional ComfortHub climate devices, as in the QML app)
 *
 * Restart and commissioning reset go through the daemon (it owns the
 * service while it runs them): the UI writes a command file, the daemon
 * reports progress in action.json.
 */
#ifndef MATTER_LOGIC_H
#define MATTER_LOGIC_H

#include <stddef.h>
#include <sys/types.h>

#define MB_APP_DIR          "/mnt/data/integrations/matter"
/* Root-only (0700) on /var/run, not in the world-writable tmp folder: the
 * command file asks the daemon to restart or reset the bridge. */
#define MB_RUN_DIR          "/var/run/freetoon-matter"
#define MB_COMMAND_PATH     MB_RUN_DIR "/command"
#define MB_ACTION_PATH      MB_RUN_DIR "/action.json"
#define MB_STATUS_PATH      "/var/volatile/tmp/toon-matter-bridge/status.json"
#define MB_PREFS_PATH       "/mnt/data/tsc/matter-bridge.json"
#define MB_COMFORT_PATH     "/var/volatile/tmp/toon-app-bus/v1/comfort-hub/state.json"
#define MB_PID_PATH         "/var/run/toon-matter-service.pid"
#define MB_SERVICE_LOG      "/var/volatile/log/toon-matter-service.log"
#define MB_SERVICE_SCRIPT   "/qmf/bin/toon-matter-service.sh"
#define MB_SERVICE_SCRIPT2  "/mnt/data/tsc/matter-service/toon-matter-service.sh"

#define MB_PREFS_SCHEMA     "nl.toon.matter-bridge/preferences-v1"
#define MB_THERMOSTAT_ID    "toon-thermostat"
#define MB_ENERGY_ID        "toon-energy-meter"
#define MB_MAX_DEVICES      16

typedef enum {
    MB_SVC_MISSING = 0,     /* no toon-matter-service.sh: bridge not installed */
    MB_SVC_STOPPED,         /* installed, no live process */
    MB_SVC_STARTING,        /* process alive, bridge has not reported yet (~1 min) */
    MB_SVC_RUNNING,         /* process alive and status.json says running */
} mb_service_t;

typedef enum {
    MB_ACT_IDLE = 0,
    MB_ACT_START,           /* daemon (re)starts a stopped service on its own */
    MB_ACT_RESTART,         /* asked from the screen */
    MB_ACT_RESET,           /* commissioning reset, asked from the screen */
} mb_action_t;

typedef enum { MB_KIND_THERMOSTAT = 0, MB_KIND_ENERGY, MB_KIND_CLIMATE } mb_kind_t;

typedef struct {
    char      id[64];
    char      name[64];
    char      detail[96];
    mb_kind_t kind;
    int       online;
    int       selected;
} mb_device_t;

typedef struct {
    /* service */
    mb_service_t service;
    int          pid;
    char         script[96];             /* the service script found, "" if missing */
    char         log_error[160];         /* last error line of the service log */

    /* status.json */
    int          status_ok;              /* file present and parsed */
    int          status_running;
    int          commissioned;
    char         message[160];
    char         pairing_code[16];       /* 11 digits, "" once commissioned */
    char         qr_payload[64];         /* "MT:...", "" once commissioned */
    int          energy_available;
    int          energy_online;
    double       import_w, export_w, net_w, gas_lph;

    /* the shareable devices with the current choice */
    int          n_devices;
    mb_device_t  devices[MB_MAX_DEVICES];
    int          n_selected;

    /* daemon (action.json) */
    int          daemon_alive;
    mb_action_t  action;                 /* running now */
    mb_action_t  last_action;            /* last finished */
    int          last_failed;
    long long    last_done;              /* unix s */
    char         last_message[160];
    int          autostart_paused;       /* daemon gave up restarting for a while */
} mb_snapshot_t;

/* File locations; the defaults are the real ones on the Toon. Tests and the
 * host preview point them elsewhere. */
typedef struct {
    const char * status;
    const char * prefs;
    const char * comfort;
    const char * pidfile;
    const char * service_log;
    const char * action;
    const char * script;      /* NULL: look for MB_SERVICE_SCRIPT, then ..._SCRIPT2 */
} mb_paths_t;

void         mb_default_paths(mb_paths_t * p);

/* Reads everything above. `en` picks English device descriptions. */
void         mb_snapshot_load(mb_snapshot_t * s, const mb_paths_t * p, int en);

/* Sets one device on or off in the preferences file and writes it back
 * atomically (other devices' entries are kept). 0 on success. */
int          mb_set_selected(const char * prefs_path, const char * id, int on);

/* The UI's request to the daemon: "restart" or "reset". 0 on success. */
int          mb_send_command(const char * path, const char * command);

/* "34970112332" -> "3497-011-2332" (4-3-4, as Apple Home and Google Home show it). */
void         mb_format_code(const char * code, char * out, size_t n);

/* Liveness of a process by pid file / pid. */
int          mb_pid_from_file(const char * path);
int          mb_pid_alive(int pid);

/* small file helpers */
char *       mb_read_file(const char * path);                 /* malloc'd, NUL-terminated */
char *       mb_read_tail(const char * path, size_t max);      /* last `max` bytes */
int          mb_write_atomic(const char * path, const char * data, int do_fsync);
int          mb_mkdirs(const char * dir);
/* Creates `dir` (mode 0700) if needed and checks it is a real directory
 * owned by this user with no group/other write access. 0 if safe. */
int          mb_secure_dir(const char * dir);
/* Reads a small file without following a symlink; malloc'd or NULL. */
char *       mb_read_file_nofollow(const char * path);

const char * mb_action_name(mb_action_t a);
mb_action_t  mb_action_from_name(const char * s);

#endif
