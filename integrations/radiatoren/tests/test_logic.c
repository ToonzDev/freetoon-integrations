/*
 * Host unit test for radiatoren_logic.c. Mirrors ../../tests/logic.test.js
 * (the JS tests of the QML app) one-for-one, plus JSON round trips.
 *
 *   make test        (or: cc -std=c99 -Wall -I.. tests/test_logic.c ../radiatoren_logic.c -lm)
 */
#include "../radiatoren_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed, g_failed;
static const char * g_test = "";

#define CHECK(cond) do { if (!(cond)) { g_failed++; \
    fprintf(stderr, "FAIL %s: %s (line %d)\n", g_test, #cond, __LINE__); } } while (0)
#define CHECK_NUM(a, b) do { double _a = (a), _b = (b); if (!(_a == _b)) { g_failed++; \
    fprintf(stderr, "FAIL %s: %s = %g, want %g (line %d)\n", g_test, #a, _a, _b, __LINE__); } } while (0)
#define CHECK_STR(a, b) do { const char * _a = (a), * _b = (b); if (strcmp(_a, _b) != 0) { g_failed++; \
    fprintf(stderr, "FAIL %s: %s = \"%s\", want \"%s\" (line %d)\n", g_test, #a, _a, _b, __LINE__); } } while (0)

static void begin(const char * name) { g_test = name; }
static void end(void) { g_passed++; printf("ok  %s\n", g_test); }

/* 2026-11-16 is a Monday. */
static struct tm at(int day, int hours, int minutes) {
    struct tm tm = { 0 };
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 16 + day;
    tm.tm_hour = hours;
    tm.tm_min = minutes;
    tm.tm_isdst = -1;
    mktime(&tm);
    return tm;
}

/* Like room(extra) in the JS test: base room, later keys override. */
static rad_settings_t g_s;
static rad_room_t * room(const char * extra) {
    char json[1024];
    snprintf(json, sizeof json,
             "{\"rooms\":[{\"id\":\"kamer1\",\"name\":\"Woonkamer\",\"comfort\":20.5,\"eco\":16,\"mode\":\"auto\","
             "\"blocks\":[{\"days\":[true,true,true,true,true,false,false],\"start\":420,\"end\":1350}]%s%s}]}",
             extra && extra[0] ? "," : "", extra ? extra : "");
    rad_settings_from_json(&g_s, json);
    return &g_s.rooms[0];
}

int main(void) {
    char buf[64];

    begin("dagindex: maandag is 0, zondag is 6");
    { struct tm a = at(0, 12, 0), b = at(6, 12, 0);
      CHECK_NUM(rad_day_index(&a), 0); CHECK_NUM(rad_day_index(&b), 6); }
    end();

    begin("temperatuur wordt begrensd en op halve graden afgerond");
    CHECK_NUM(rad_clamp_temp(20.26), 20.5);
    CHECK_NUM(rad_clamp_temp(2), 6);
    CHECK_NUM(rad_clamp_temp(99), 28);
    CHECK_NUM(rad_clamp_temp(NAN), 6);
    { rad_room_t * r = room("\"comfort\":\"abc\""); CHECK_NUM(r->comfort, 6); }   /* clampTemp("abc") */
    end();

    begin("weergave van temperatuur en tijd");
    rad_format_temp(20.5, buf, sizeof buf); CHECK_STR(buf, "20,5");
    rad_format_temp(NAN, buf, sizeof buf); CHECK_STR(buf, "--");
    rad_format_minutes(420, buf, sizeof buf); CHECK_STR(buf, "07:00");
    rad_format_minutes(1350, buf, sizeof buf); CHECK_STR(buf, "22:30");
    end();

    begin("schema: comfort binnen het blok, eco erbuiten");
    { rad_room_t * r = room(NULL);
      struct tm t1 = at(0, 6, 59), t2 = at(0, 7, 0), t3 = at(0, 22, 29), t4 = at(0, 22, 30);
      rad_target_t x;
      x = rad_target_for(r, &t1); CHECK_NUM(x.target, 16); CHECK(x.reason == RAD_REASON_ECO);
      x = rad_target_for(r, &t2); CHECK_NUM(x.target, 20.5); CHECK(x.reason == RAD_REASON_COMFORT);
      x = rad_target_for(r, &t3); CHECK_NUM(x.target, 20.5); CHECK(x.reason == RAD_REASON_COMFORT);
      x = rad_target_for(r, &t4); CHECK_NUM(x.target, 16); CHECK(x.reason == RAD_REASON_ECO); }
    end();

    begin("schema: dag die niet is aangevinkt blijft op eco");
    { rad_room_t * r = room(NULL); struct tm t = at(5, 12, 0); CHECK_NUM(rad_target_for(r, &t).target, 16); }
    end();

    begin("handmatig en uit gaan voor het schema");
    { rad_room_t * r = room("\"mode\":\"manual\",\"manualTemp\":22"); struct tm t = at(0, 3, 0);
      rad_target_t x = rad_target_for(r, &t);
      CHECK_NUM(x.target, 22); CHECK_STR(rad_reason_str(x.reason), "handmatig");
      r = room("\"mode\":\"off\""); t = at(0, 12, 0); x = rad_target_for(r, &t);
      CHECK_NUM(x.target, 6); CHECK_STR(rad_reason_str(x.reason), "uit"); }
    end();

    begin("eerstvolgende wissel vandaag");
    { rad_room_t * r = room(NULL);
      struct tm a = at(0, 6, 0), b = at(0, 12, 0), c = at(0, 23, 0), d = at(5, 6, 0);
      CHECK_NUM(rad_next_change_today(r, &a), 420);
      CHECK_NUM(rad_next_change_today(r, &b), 1350);
      CHECK_NUM(rad_next_change_today(r, &c), -1);
      CHECK_NUM(rad_next_change_today(r, &d), -1);
      r = room("\"mode\":\"manual\""); CHECK_NUM(rad_next_change_today(r, &a), -1); }
    end();

    begin("blok wordt gerepareerd: eind na begin, kwartieren, zeven dagen");
    { rad_room_t * r = room("\"blocks\":[{\"days\":[true],\"start\":607,\"end\":300}]");
      rad_block_t * b = &r->blocks[0];
      CHECK_NUM(b->days[0], 1); CHECK_NUM(b->days[1], 0); CHECK_NUM(b->days[6], 0);
      CHECK_NUM(b->start, 600); CHECK_NUM(b->end, 615); }
    end();

    begin("instellingen: kapotte invoer levert geldige lege instellingen");
    { rad_settings_t s;
      rad_settings_from_json(&s, NULL); CHECK_NUM(s.n_rooms, 0); CHECK_NUM(s.boiler_control, 0);
      rad_settings_from_json(&s, "{\"rooms\":\"x\",\"boilerControl\":\"ja\"}");
      CHECK_NUM(s.n_rooms, 0); CHECK_NUM(s.boiler_control, 0);
      rad_settings_from_json(&s, "{kapot"); CHECK_NUM(s.n_rooms, 0); CHECK_NUM(s.hysteresis, 0.5); }
    end();

    begin("instellingen: dubbele kamer-id's worden uniek, maximaal vier blokken");
    { rad_settings_t s; const char * blk = "{\"days\":[true,true,true,true,true,false,false],\"start\":420,\"end\":1350}";
      char json[1024];
      snprintf(json, sizeof json, "{\"rooms\":[{\"id\":\"a\",\"blocks\":[%s,%s,%s,%s,%s,%s]},{\"id\":\"a\"}]}",
               blk, blk, blk, blk, blk, blk);
      rad_settings_from_json(&s, json);
      CHECK(strcmp(s.rooms[0].id, s.rooms[1].id) != 0);
      CHECK_STR(s.rooms[1].id, "ax");
      CHECK_NUM(s.rooms[0].n_blocks, 4); }
    end();

    begin("nieuw kamer-id telt door");
    { rad_settings_t s; memset(&s, 0, sizeof s); char id[32];
      rad_next_room_id(&s, id, sizeof id); CHECK_STR(id, "kamer1");
      s.n_rooms = 2; snprintf(s.rooms[0].id, sizeof s.rooms[0].id, "kamer2"); snprintf(s.rooms[1].id, sizeof s.rooms[1].id, "iets");
      rad_next_room_id(&s, id, sizeof id); CHECK_STR(id, "kamer3"); }
    end();

    begin("warmtevraag: alleen met meting en onder doel min hysterese");
    { struct tm noon = at(0, 12, 0);
      CHECK_NUM(rad_room_demands_heat(room(NULL), &noon, 0.5), 0);
      CHECK_NUM(rad_room_demands_heat(room("\"measured\":19.9"), &noon, 0.5), 1);
      CHECK_NUM(rad_room_demands_heat(room("\"measured\":20.0"), &noon, 0.5), 0);
      CHECK_NUM(rad_room_demands_heat(room("\"measured\":5,\"mode\":\"off\""), &noon, 0.5), 0);
      rad_settings_t s; int idx[4];
      rad_settings_from_json(&s, "{\"rooms\":["
        "{\"id\":\"a\",\"comfort\":20.5,\"measured\":18,\"blocks\":[{\"days\":[true,true,true,true,true,false,false],\"start\":420,\"end\":1350}]},"
        "{\"id\":\"b\",\"comfort\":20.5,\"measured\":21,\"blocks\":[{\"days\":[true,true,true,true,true,false,false],\"start\":420,\"end\":1350}]},"
        "{\"id\":\"c\"}]}");
      int n = rad_demanding_rooms(&s, &noon, 0.5, idx, 4);
      CHECK_NUM(n, 1); CHECK_STR(s.rooms[idx[0]].id, "a"); }
    end();

    begin("keteladvies: niets zonder metingen");
    { struct tm noon = at(0, 12, 0); double out; rad_settings_t s;
      room(NULL); CHECK_NUM(rad_boiler_setpoint_advice(&g_s, &noon, 0.5, 19, &out), 0);
      rad_settings_from_json(&s, "{\"rooms\":[]}");
      CHECK_NUM(rad_boiler_setpoint_advice(&s, &noon, 0.5, 19, &out), 0); }
    end();

    begin("keteladvies: bij warmtevraag boven de temperatuur bij de Toon");
    { struct tm noon = at(0, 12, 0); double out = 0;
      room("\"measured\":18");
      CHECK_NUM(rad_boiler_setpoint_advice(&g_s, &noon, 0.5, 19.2, &out), 1); CHECK_NUM(out, 21);
      CHECK_NUM(rad_boiler_setpoint_advice(&g_s, &noon, 0.5, 27.9, &out), 1); CHECK_NUM(out, 28);
      CHECK_NUM(rad_boiler_setpoint_advice(&g_s, &noon, 0.5, NAN, &out), 0); }
    end();

    begin("keteladvies: zonder warmtevraag het laagste eco-setpoint");
    { struct tm noon = at(0, 12, 0); double out = 0; rad_settings_t s;
      rad_settings_from_json(&s, "{\"rooms\":["
        "{\"id\":\"a\",\"measured\":21,\"eco\":16,\"comfort\":20.5},"
        "{\"id\":\"b\",\"measured\":21,\"eco\":14.5,\"comfort\":20.5}]}");
      CHECK_NUM(rad_boiler_setpoint_advice(&s, &noon, 0.5, 19, &out), 1); CHECK_NUM(out, 14.5); }
    end();

    begin("publicatie voor Home Assistant geeft per kamer het doel van nu");
    { rad_settings_t s; struct tm t = at(0, 8, 0); time_t now = mktime(&t);
      rad_settings_from_json(&s, "{\"rooms\":["
        "{\"id\":\"kamer1\",\"name\":\"Woonkamer\",\"comfort\":20.5,\"eco\":16,\"mode\":\"auto\","
        "\"blocks\":[{\"days\":[true,true,true,true,true,false,false],\"start\":420,\"end\":1350}]},"
        "{\"id\":\"kamer2\",\"name\":\"Slaapkamer\",\"mode\":\"off\"}]}");
      char * p = rad_publish_json(&s, now, &t);
      char want[512];
      snprintf(want, sizeof want,
               "{\"schema\":\"nl.toon.radiatoren/targets-v1\",\"updated\":%lld,\"hysteresis\":0.5,\"rooms\":["
               "{\"id\":\"kamer1\",\"name\":\"Woonkamer\",\"target\":20.5,\"mode\":\"auto\",\"reason\":\"comfort\"},"
               "{\"id\":\"kamer2\",\"name\":\"Slaapkamer\",\"target\":6,\"mode\":\"off\",\"reason\":\"uit\"}]}",
               (long long)now);
      CHECK_STR(p, want);
      free(p); }
    end();

    /* ---- beyond the JS tests -------------------------------------------- */

    begin("instellingenbestand van de QML-app: lezen, schrijven, opnieuw lezen");
    { const char * qml =
        "{\n  \"schema\": \"nl.toon.radiatoren/settings-v1\",\n  \"boilerControl\": false,\n  \"hysteresis\": 0.5,\n"
        "  \"rooms\": [\n    {\n      \"id\": \"kamer1\",\n      \"name\": \"Woonkamer \\\"groot\\\" \\u00e9\",\n"
        "      \"valveUuid\": \"\",\n      \"comfort\": 20.5,\n      \"eco\": 16,\n      \"mode\": \"manual\",\n"
        "      \"manualTemp\": 21,\n      \"blocks\": [\n        {\n          \"days\": [\n            true,\n"
        "            true,\n            true,\n            true,\n            true,\n            false,\n"
        "            false\n          ],\n          \"start\": 420,\n          \"end\": 1350\n        }\n      ]\n"
        "    },\n    {\n      \"id\": \"kamer2\",\n      \"name\": \"Zolder\",\n      \"valveUuid\": \"\",\n"
        "      \"comfort\": 19,\n      \"eco\": 15.5,\n      \"mode\": \"auto\",\n      \"manualTemp\": 20,\n"
        "      \"blocks\": []\n    }\n  ]\n}";
      rad_settings_t a, b;
      rad_settings_from_json(&a, qml);
      CHECK_NUM(a.n_rooms, 2);
      CHECK_STR(a.rooms[0].name, "Woonkamer \"groot\" \xc3\xa9");
      CHECK(a.rooms[0].mode == RAD_MODE_MANUAL);
      CHECK_NUM(a.rooms[0].manual_temp, 21);
      CHECK_NUM(a.rooms[1].eco, 15.5);
      char * out = rad_settings_to_json(&a);
      /* Same text as JSON.stringify(.., null, 2), apart from the \u00e9 the
       * QML app writes as a raw UTF-8 character. */
      char * expect = strdup(qml);
      char * u = strstr(expect, "\\u00e9");
      memcpy(u, "\xc3\xa9", 2); memmove(u + 2, u + 6, strlen(u + 6) + 1);
      CHECK_STR(out, expect);
      rad_settings_from_json(&b, out);
      CHECK(memcmp(&a, &b, sizeof a) == 0);
      free(out); free(expect); }
    end();

    begin("bewerkingen uit RadiatorenApp.qml");
    { rad_settings_t s; char id[32]; struct tm noon = at(0, 12, 0);
      rad_settings_from_json(&s, NULL);
      CHECK_NUM(rad_add_room(&s, id, sizeof id), 1); CHECK_STR(id, "kamer1");
      CHECK_STR(s.rooms[0].name, "Kamer 1"); CHECK_NUM(s.rooms[0].n_blocks, 1);
      rad_add_room(&s, id, sizeof id); CHECK_STR(s.rooms[1].name, "Kamer 2"); CHECK_STR(id, "kamer2");
      CHECK_NUM(rad_rename_room(&s, 0, "   "), 0);
      CHECK_NUM(rad_rename_room(&s, 0, "  Een hele lange kamernaam van meer dan 24  "), 1);
      CHECK_STR(s.rooms[0].name, "Een hele lange kamernaam");
      /* plus/min: from the schedule (comfort 20 at noon on Monday) to manual */
      CHECK_NUM(rad_nudge_target(&s, 0, 0.5, &noon), 1);
      CHECK(s.rooms[0].mode == RAD_MODE_MANUAL); CHECK_NUM(s.rooms[0].manual_temp, 20.5);
      /* cycle: manual -> off -> auto -> manual (manual starts at the schedule target) */
      rad_cycle_mode(&s, 0, &noon); CHECK(s.rooms[0].mode == RAD_MODE_OFF);
      rad_nudge_target(&s, 0, -0.5, &noon); CHECK_NUM(s.rooms[0].manual_temp, 20);   /* from "off": last manual value */
      rad_set_mode(&s, 0, RAD_MODE_AUTO, &noon);
      rad_cycle_mode(&s, 0, &noon); CHECK(s.rooms[0].mode == RAD_MODE_MANUAL); CHECK_NUM(s.rooms[0].manual_temp, 20);
      CHECK_NUM(rad_nudge_comfort(&s, 0, 100), 1); CHECK_NUM(s.rooms[0].comfort, 28);
      CHECK_NUM(rad_nudge_comfort(&s, 0, 0.5), 0);
      CHECK_NUM(rad_nudge_eco(&s, 0, -0.5), 1); CHECK_NUM(s.rooms[0].eco, 15.5);
      for (int i = 0; i < 5; i++) rad_add_block(&s, 0);
      CHECK_NUM(s.rooms[0].n_blocks, 4);
      CHECK_NUM(rad_remove_block(&s, 0, 9), 0);
      CHECK_NUM(rad_remove_block(&s, 0, 0), 1); CHECK_NUM(s.rooms[0].n_blocks, 3);
      CHECK_NUM(rad_toggle_block_day(&s, 0, 0, 6), 1); CHECK_NUM(s.rooms[0].blocks[0].days[6], 1);
      CHECK_NUM(rad_nudge_block_time(&s, 0, 0, 0, 15), 1); CHECK_NUM(s.rooms[0].blocks[0].start, 435);
      CHECK_NUM(rad_nudge_block_time(&s, 0, 0, 1, 90), 1); CHECK_NUM(s.rooms[0].blocks[0].end, 1440);
      CHECK_NUM(rad_nudge_block_time(&s, 0, 0, 1, 15), 0);     /* already at 24:00 */
      s.rooms[0].blocks[0].start = 1425;
      CHECK_NUM(rad_nudge_block_time(&s, 0, 0, 0, 15), 0);     /* start would reach the end */
      CHECK_NUM(rad_remove_room(&s, 0), 1); CHECK_NUM(s.n_rooms, 1); CHECK_STR(s.rooms[0].id, "kamer2");
      rad_add_room(&s, id, sizeof id); CHECK_STR(id, "kamer3"); }
    end();

    printf("%d tests geslaagd, %d controles mislukt\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
