/*
 * Host unit test for matter_json.c and matter_logic.c:  make test
 */
#include "../matter_json.h"
#include "../matter_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static char g_dir[256];
static char * path(const char * name) {
    static char buf[8][320];
    static int i;
    i = (i + 1) % 8;
    snprintf(buf[i], sizeof buf[i], "%s/%s", g_dir, name);
    return buf[i];
}
static void put(const char * name, const char * text) {
    FILE * f = fopen(path(name), "w");
    fputs(text, f);
    fclose(f);
}

static void test_json(void) {
    mj_t * r = mj_parse(" {\"a\": [1, 2.5, -3e2], \"s\": \"x\\\"y\\u00e9\\ud83d\\ude00\", \"t\": true, \"f\": false, \"n\": null, \"o\": {}} ");
    CHECK(r != NULL);
    const mj_t * a = mj_get(r, "a");
    CHECK(a && a->type == MJ_ARR && a->child && a->child->num == 1 && a->child->next->num == 2.5
          && a->child->next->next->num == -300);
    CHECK(!strcmp(mj_str(mj_get(r, "s"), ""), "x\"y\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(mj_is_true(mj_get(r, "t")) && !mj_is_true(mj_get(r, "f")) && !mj_is_true(mj_get(r, "n")));
    CHECK(mj_get(r, "o") && mj_get(r, "o")->type == MJ_OBJ && !mj_get(r, "o")->child);
    CHECK(mj_get(r, "missing") == NULL);
    CHECK(mj_num(mj_get(r, "s"), 7) == 7);
    mj_free(r);

    CHECK(mj_parse("") == NULL);
    CHECK(mj_parse("{") == NULL);
    CHECK(mj_parse("{\"a\":1,}") == NULL);
    CHECK(mj_parse("[1 2]") == NULL);
    CHECK(mj_parse("{} x") == NULL);
    CHECK(mj_parse("\"tab\there\"") == NULL);       /* raw control char */
    char deep[200];
    memset(deep, '[', 100); memset(deep + 100, ']', 99); deep[199] = 0;
    CHECK(mj_parse(deep) == NULL);                    /* unbalanced and too deep */

    char q[64];
    size_t need = mj_quote("a\"b\\c\n", q, sizeof q);
    CHECK(!strcmp(q, "\"a\\\"b\\\\c\\n\"") && need == strlen(q));
    need = mj_quote("abcdef", q, 4);
    CHECK(need == 8 && strlen(q) == 3);
}

static void test_format(void) {
    char out[32];
    mb_format_code("34970112332", out, sizeof out);
    CHECK(!strcmp(out, "3497-011-2332"));
    mb_format_code("1234", out, sizeof out);
    CHECK(!strcmp(out, "1234"));
    mb_format_code(NULL, out, sizeof out);
    CHECK(!strcmp(out, ""));
    CHECK(mb_action_from_name(mb_action_name(MB_ACT_RESET)) == MB_ACT_RESET);
    CHECK(mb_action_from_name("bogus") == MB_ACT_IDLE);
}

static mb_paths_t paths(void) {
    mb_paths_t p;
    mb_default_paths(&p);
    /* own copies: path() reuses a small ring of buffers */
    p.status = strdup(path("status.json"));
    p.prefs = strdup(path("prefs.json"));
    p.comfort = strdup(path("comfort.json"));
    p.pidfile = strdup(path("service.pid"));
    p.service_log = strdup(path("service.log"));
    p.action = strdup(path("action.json"));
    p.script = strdup(path("service.sh"));
    return p;
}

/* Setup code and QR payload: the Matter specification's example device
 * (passcode 20202021, discriminator 3840), not a real bridge. */
static const char * STATUS =
    "{\n  \"schema\": \"nl.toon.matter-bridge/status-v1\",\n  \"running\": true,\n  \"commissioned\": false,\n"
    "  \"message\": \"1 apparaat via Matter gedeeld\",\n  \"pairing_code\": \"34970112332\",\n"
    "  \"qr_payload\": \"MT:Y.K9042C00KA0648G00\",\n  \"selected_count\": 1,\n"
    "  \"energy\": {\"available\": true, \"online\": true, \"import_power_w\": 0, \"export_power_w\": 812,"
    " \"net_power_w\": -812, \"gas_flow_lph\": 0}\n}\n";

static void test_snapshot(void) {
    mb_paths_t p = paths();
    mb_snapshot_t s;

    /* nothing installed */
    mb_snapshot_load(&s, &p, 0);
    CHECK(s.service == MB_SVC_MISSING);
    CHECK(s.n_devices == 1 && !strcmp(s.devices[0].id, MB_THERMOSTAT_ID) && !s.devices[0].selected);
    CHECK(!s.daemon_alive);

    /* installed but stopped: stale status must not offer a code */
    put("service.sh", "#!/bin/sh\n");
    chmod(path("service.sh"), 0755);
    put("status.json", STATUS);
    put("service.log", "[09:00:00.000] [Matterbridge] [error] Bind failed: EADDRINUSE\n[09:00:01.000] ok\n"
                       "[09:00:02.000] [Matterbridge] [error] Error getting global node_modules directory: npm root -g\n"
                       "Error: Command failed: npm root -g\n");
    mb_snapshot_load(&s, &p, 0);
    CHECK(s.service == MB_SVC_STOPPED);
    CHECK(s.pairing_code[0] == 0 && s.qr_payload[0] == 0);
    CHECK(!strcmp(s.log_error, "Bind failed: EADDRINUSE"));

    /* running (this test process stands in for node) */
    char pid[32];
    snprintf(pid, sizeof pid, "%d\n", (int)getpid());
    put("service.pid", pid);
    put("prefs.json", "{\"schema\":\"" MB_PREFS_SCHEMA "\",\"selected_devices\":{\"toon-thermostat\":true,\"gone\":true}}");
    put("comfort.json", "{\"updated_at\":\"x\",\"state\":{\"devices\":["
                        "{\"id\":\"mhi-1\",\"name\":\"Airco boven\",\"matter_eligible\":true,\"matter_type\":\"thermostat\","
                        "\"brand\":\"MHI\",\"technology\":\"WF-RAC\",\"online\":true},"
                        "{\"id\":\"x\",\"name\":\"Fan\",\"matter_eligible\":true,\"matter_type\":\"fan\"},"
                        "{\"id\":7,\"name\":\"Numeric\",\"matter_eligible\":true,\"matter_type\":\"thermostat\"},"
                        "{\"id\":\"no\",\"name\":\"Not eligible\",\"matter_eligible\":false,\"matter_type\":\"thermostat\"}]}}");
    mb_snapshot_load(&s, &p, 0);
    CHECK(s.service == MB_SVC_RUNNING);
    CHECK(s.status_ok && s.status_running && !s.commissioned);
    CHECK(!strcmp(s.pairing_code, "34970112332") && !strcmp(s.qr_payload, "MT:Y.K9042C00KA0648G00"));
    CHECK(s.log_error[0] == 0);
    CHECK(s.n_devices == 4);
    CHECK(!strcmp(s.devices[1].id, "mhi-1") && !strcmp(s.devices[1].detail, "MHI WF-RAC, Matter-thermostaat"));
    CHECK(!strcmp(s.devices[2].id, "7") && !s.devices[2].online);
    CHECK(!strcmp(s.devices[3].id, MB_ENERGY_ID) && !strcmp(s.devices[3].detail, "Elektriciteit 812 W teruglevering"));
    CHECK(s.n_selected == 1 && s.devices[0].selected);

    /* English descriptions */
    mb_snapshot_load(&s, &p, 1);
    CHECK(!strcmp(s.devices[3].name, "Toon energy meter") && !strcmp(s.devices[3].detail, "Electricity 812 W export"));

    /* commissioned: the bridge clears the code itself */
    put("status.json", "{\"running\":true,\"commissioned\":true,\"pairing_code\":\"\",\"qr_payload\":\"\",\"energy\":{\"available\":false}}");
    mb_snapshot_load(&s, &p, 0);
    CHECK(s.service == MB_SVC_RUNNING && s.commissioned && !s.pairing_code[0] && s.n_devices == 3);

    /* running:false while the process lives = still starting */
    put("status.json", "{\"running\":false}");
    mb_snapshot_load(&s, &p, 0);
    CHECK(s.service == MB_SVC_STARTING);

    /* daemon state */
    char act[256];
    snprintf(act, sizeof act, "{\"daemon_pid\":%d,\"action\":\"reset\",\"last_action\":\"restart\","
             "\"last_failed\":true,\"last_done\":1763400000,\"last_message\":\"stop faalde\",\"autostart_paused\":false}",
             (int)getpid());
    put("action.json", act);
    mb_snapshot_load(&s, &p, 0);
    CHECK(s.daemon_alive && s.action == MB_ACT_RESET && s.last_action == MB_ACT_RESTART && s.last_failed);
    CHECK(s.last_done == 1763400000 && !strcmp(s.last_message, "stop faalde"));
    put("action.json", "{\"daemon_pid\":999999,\"action\":\"reset\"}");
    mb_snapshot_load(&s, &p, 0);
    CHECK(!s.daemon_alive && s.action == MB_ACT_IDLE);
}

static void test_prefs(void) {
    mb_paths_t p = paths();
    unlink(p.prefs);
    CHECK(mb_set_selected(p.prefs, MB_ENERGY_ID, 1) == 0);
    char * t = mb_read_file(p.prefs);
    CHECK(t && !strcmp(t, "{\n  \"schema\": \"nl.toon.matter-bridge/preferences-v1\",\n  \"selected_devices\": {\n"
                         "    \"toon-energy-meter\": true\n  }\n}\n"));
    free(t);

    put("prefs.json", "{\"schema\":\"" MB_PREFS_SCHEMA "\",\"selected_devices\":{\"toon-thermostat\":true,\"a\\\"b\":false},\"extra\":1}");
    CHECK(mb_set_selected(p.prefs, MB_THERMOSTAT_ID, 0) == 0);
    CHECK(mb_set_selected(p.prefs, MB_ENERGY_ID, 1) == 0);
    t = mb_read_file(p.prefs);
    mj_t * r = mj_parse(t);
    CHECK(r != NULL);
    const mj_t * m = mj_get(r, "selected_devices");
    CHECK(!strcmp(mj_str(mj_get(r, "schema"), ""), MB_PREFS_SCHEMA));
    CHECK(mj_get(m, "toon-thermostat") && !mj_is_true(mj_get(m, "toon-thermostat")));
    CHECK(mj_is_true(mj_get(m, MB_ENERGY_ID)));
    CHECK(mj_get(m, "a\"b") && !mj_is_true(mj_get(m, "a\"b")));
    mj_free(r);
    free(t);

    /* corrupt file: start over rather than fail */
    put("prefs.json", "{not json");
    CHECK(mb_set_selected(p.prefs, MB_THERMOSTAT_ID, 1) == 0);
    t = mb_read_file(p.prefs);
    r = mj_parse(t);
    CHECK(r && mj_is_true(mj_get(mj_get(r, "selected_devices"), MB_THERMOSTAT_ID)));
    mj_free(r);
    free(t);

    CHECK(mb_set_selected(p.prefs, "", 1) != 0);
}

static void test_command(void) {
    char cmd[320], dir[320], link[320], target[320];
    snprintf(cmd, sizeof cmd, "%s/run/sub/command", g_dir);
    CHECK(mb_send_command(cmd, "reset") == 0);
    char * t = mb_read_file_nofollow(cmd);
    CHECK(t && !strcmp(t, "reset\n"));
    free(t);
    snprintf(dir, sizeof dir, "%s/run/sub", g_dir);
    struct stat st;
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);

    /* a folder someone else could write gets tightened */
    chmod(dir, 0777);
    CHECK(mb_secure_dir(dir) == 0 && stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);

    /* a symlinked run folder is refused */
    snprintf(target, sizeof target, "%s/elsewhere", g_dir);
    snprintf(link, sizeof link, "%s/run/linked", g_dir);
    mb_mkdirs(target);
    CHECK(symlink(target, link) == 0);
    snprintf(cmd, sizeof cmd, "%s/command", link);
    CHECK(mb_secure_dir(link) != 0);
    CHECK(mb_send_command(cmd, "reset") != 0);

    /* a symlinked command file is not followed */
    snprintf(cmd, sizeof cmd, "%s/run/sub/command", g_dir);
    unlink(cmd);
    put("secret.txt", "restart\n");
    CHECK(symlink(path("secret.txt"), cmd) == 0);
    CHECK(mb_read_file_nofollow(cmd) == NULL);

    /* a planted file or symlink at the temp name is never written through */
    char tmpname[400];
    snprintf(tmpname, sizeof tmpname, "%s.tmp-%d", path("victim.json"), (int)getpid());
    put("target.txt", "keep\n");
    CHECK(symlink(path("target.txt"), tmpname) == 0);
    CHECK(mb_write_atomic(path("victim.json"), "new\n", 0) == 0);
    t = mb_read_file(path("target.txt"));
    CHECK(t && !strcmp(t, "keep\n"));
    free(t);
    t = mb_read_file(path("victim.json"));
    CHECK(t && !strcmp(t, "new\n"));
    free(t);
}

int main(void) {
    const char * tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/matter-test-%d", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    mb_mkdirs(g_dir);
    test_json();
    test_format();
    test_snapshot();
    test_prefs();
    test_command();
    char rm[300];
    snprintf(rm, sizeof rm, "rm -rf '%s'", g_dir);
    if (system(rm) != 0) fprintf(stderr, "cleanup failed\n");
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
