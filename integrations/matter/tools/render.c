/*
 * render.c - host preview of the Matter plugin, no Toon needed.
 *
 * Links screen_matter.c against the real LVGL + fonts + lv_conf.h of the
 * FreeToon tree, renders into a 1024x600 framebuffer and writes a PPM. Taps
 * are injected through an LVGL pointer device, so they take the same code
 * paths as a finger on the Toon.
 *
 *   render OUT.ppm [--light] [--theme N] [--en] [X,Y | wait:MS]...
 *   env MATTER_ROOT=<folder with status.json, prefs.json, ...>
 */
#include "lvgl/lvgl.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

settings_t settings;                    /* toonui's global; the plugin reads two fields */

void ui_push(lv_obj_t * scr) { lv_scr_load(scr); }
void ui_push_owned(lv_obj_t * scr) { lv_scr_load(scr); }
void ui_pop(void) { fprintf(stderr, "[render] ui_pop()\n"); }

lv_obj_t * screen_create(void);
#ifdef MB_HAVE_THEMES
void theme_set(int id);                 /* theme.c, FreeToon 1.1+ */
/* theme.c's automatic day/night theme asks toonui for sunrise/sunset; the
 * preview has no location, so: unknown (weak, in case a tree provides it). */
__attribute__((weak)) int sun_times_local(int * rise_min, int * set_min) {
    (void)rise_min; (void)set_min;
    return -1;
}
#endif

#define W 1024
#define H 600
static lv_color_t s_fb[W * H];
static lv_color_t s_draw[W * H];

static void flush(lv_disp_drv_t * drv, const lv_area_t * a, lv_color_t * px) {
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&s_fb[y * W + a->x1], px + (y - a->y1) * (a->x2 - a->x1 + 1),
               (size_t)(a->x2 - a->x1 + 1) * sizeof(lv_color_t));
    lv_disp_flush_ready(drv);
}

static int s_px, s_py, s_pressed;
static void touch(lv_indev_drv_t * drv, lv_indev_data_t * d) {
    (void)drv;
    d->point.x = (lv_coord_t)s_px;
    d->point.y = (lv_coord_t)s_py;
    d->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void pump(int ms) {
    for (; ms > 0; ms -= 10) { lv_tick_inc(10); lv_timer_handler(); }
}

int main(int argc, char ** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s OUT.ppm [--light] [--en] [X,Y|wait:MS]...\n", argv[0]); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--light")) settings.home_light_mode = 1;
        if (!strcmp(argv[i], "--en")) settings.language = 1;
    }
#ifdef MB_HAVE_THEMES
    int theme = settings.home_light_mode ? 1 : 0;
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--theme")) theme = atoi(argv[i + 1]);
    theme_set(theme);
#endif

    lv_init();
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf, s_draw, NULL, W * H);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.hor_res = W;
    dd.ver_res = H;
    dd.flush_cb = flush;
    dd.draw_buf = &buf;
    dd.full_refresh = 1;
    lv_disp_drv_register(&dd);
    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
    id.read_cb = touch;
    lv_indev_drv_register(&id);

    lv_scr_load(screen_create());
    pump(300);

    for (int i = 2; i < argc; i++) {
        int x, y;
        if (sscanf(argv[i], "%d,%d", &x, &y) == 2) {
            s_px = x; s_py = y;
            s_pressed = 1; pump(80);
            s_pressed = 0; pump(400);
        } else if (!strncmp(argv[i], "wait:", 5)) {
            pump(atoi(argv[i] + 5));
        }
    }
    pump(100);

    FILE * f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = { s_fb[i].ch.red, s_fb[i].ch.green, s_fb[i].ch.blue };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}
