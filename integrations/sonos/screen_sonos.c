/*
 * screen_sonos.c — Full-screen Sonos Player & Favorites modal.
 * Provides room selector, playback state, transport controls, volume slider,
 * and a browsable list of Sonos favorites / radio stations for instant playback.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "sonos.h"
#include "settings.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static lv_obj_t * s_modal = NULL;
static lv_obj_t * s_dd_zone = NULL;
static lv_obj_t * s_dd_source = NULL;
static lv_obj_t * s_lbl_status = NULL;
static lv_obj_t * s_lbl_title = NULL;
static lv_obj_t * s_lbl_artist = NULL;
static lv_obj_t * s_lbl_album = NULL;
static lv_obj_t * s_btn_play = NULL;
static lv_obj_t * s_lbl_play = NULL;
static lv_obj_t * s_art_box = NULL;
static lv_obj_t * s_img_art = NULL;
static lv_obj_t * s_slider_pos = NULL;
static lv_obj_t * s_lbl_time = NULL;
static lv_obj_t * s_pos_container = NULL;
static lv_obj_t * s_lbl_vol = NULL;
static lv_obj_t * s_fav_list = NULL;
static lv_obj_t * s_lbl_fav_msg = NULL;
static lv_timer_t * s_refresh_timer = NULL;

/* Forward declarations */
static void open_playlist_tracks_view(const char * name, const char * id_or_uri, sonos_source_t src);
static void close_playlist_tracks_view(void);
static void populate_tracks_list(void);
static void open_search_view(void);
static void close_search_view(void);
static void populate_search_results(void);
static void on_search_click(lv_event_t * e);

static bool s_pos_dragging = false;
static int  s_art_last_ver = -1;
static char     s_pending_name[64] = "";
static uint32_t s_pending_until = 0;
static uint32_t s_last_item_click = 0;

/* Multi-source items loading (Favorites / Sonos Playlists / Spotify) */
static sonos_spotify_cfg_t s_spot_cfg = {0};
static sonos_source_t      s_cur_source = SONOS_SRC_FAVORITES;
static int                 s_cur_spot_user = 0;
static sonos_item_t        s_items[SONOS_MAX_ITEMS];
static int                 s_item_count = 0;
static volatile int        s_items_loading = 0;
static volatile int        s_items_ready = 0;
static volatile int        s_tracks_ready = 0;
static volatile int        s_search_ready = 0;

static void * items_loader_thread(void * arg) {
    (void)arg;
    int n = sonos_get_source_items(s_cur_source, s_cur_spot_user, s_items, SONOS_MAX_ITEMS);
    s_item_count = n;
    s_items_ready = 1;
    s_items_loading = 0;
    return NULL;
}

static void trigger_load_items(void) {
    if (s_items_loading) return;
    s_items_loading = 1;
    s_items_ready = 0;
    pthread_t tid;
    if (pthread_create(&tid, NULL, items_loader_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        s_items_loading = 0;
    }
}

static void on_close_click(lv_event_t * e) {
    (void)e;
    if (s_refresh_timer) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    close_playlist_tracks_view();
    close_search_view();
    sonos_stop();
    ui_pop();
    if (s_modal) {
        lv_obj_t * m = s_modal;
        s_modal = NULL;
        lv_obj_del(m);
    }
    s_art_box = NULL;
    s_img_art = NULL;
    s_slider_pos = NULL;
    s_lbl_time = NULL;
    s_pos_container = NULL;
    s_lbl_vol = NULL;
    s_lbl_status = NULL;
    s_lbl_title = NULL;
    s_lbl_artist = NULL;
    s_lbl_album = NULL;
    s_btn_play = NULL;
    s_lbl_play = NULL;
    s_fav_list = NULL;
    s_lbl_fav_msg = NULL;
    s_pos_dragging = false;
    s_art_last_ver = -1;
    s_pending_name[0] = 0;
    s_pending_until = 0;
}

static void on_settings_click(lv_event_t * e) {
    (void)e;
    char rooms[SONOS_MAX_ZONES][48];
    int n_rooms = sonos_probe(rooms, SONOS_MAX_ZONES);
    if (n_rooms > 0 && s_dd_zone) {
        char opts[1024] = "";
        int sel_idx = 0;
        for (int i = 0; i < n_rooms; i++) {
            if (opts[0]) strncat(opts, "\n", sizeof opts - strlen(opts) - 1);
            strncat(opts, rooms[i], sizeof opts - strlen(opts) - 1);
            if (settings.sonos_zone[0] && strcmp(settings.sonos_zone, rooms[i]) == 0)
                sel_idx = i;
        }
        lv_dropdown_set_options(s_dd_zone, opts);
        lv_dropdown_set_selected(s_dd_zone, sel_idx);
    }
}

static void on_play_pause_click(lv_event_t * e) {
    (void)e;
    sonos_play_pause();
}

static void on_prev_click(lv_event_t * e) {
    (void)e;
    sonos_prev();
}

static void on_next_click(lv_event_t * e) {
    (void)e;
    sonos_next();
}

static void on_vol_down_click(lv_event_t * e) {
    (void)e;
    sonos_volume_down();
}

static void on_vol_up_click(lv_event_t * e) {
    (void)e;
    sonos_volume_up();
}

static void format_time(int sec, char * out, size_t outsz) {
    if (sec < 0) sec = 0;
    int m = sec / 60;
    int s = sec % 60;
    snprintf(out, outsz, "%02d:%02d", m, s);
}

static void on_pos_slider_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if (code == LV_EVENT_PRESSED) {
        s_pos_dragging = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_pos_dragging) {
            s_pos_dragging = false;
            int val = lv_slider_get_value(slider);
            sonos_seek(val);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        int val = lv_slider_get_value(slider);
        sonos_state_t st;
        sonos_get_snapshot(&st);
        char e_buf[16], d_buf[16];
        format_time(val, e_buf, sizeof e_buf);
        format_time(st.duration, d_buf, sizeof d_buf);
        if (s_lbl_time) lv_label_set_text_fmt(s_lbl_time, "%s / %s", e_buf, d_buf);
    }
}

static void on_zone_change(lv_event_t * e) {
    lv_obj_t * dd = lv_event_get_target(e);
    char buf[48];
    lv_dropdown_get_selected_str(dd, buf, sizeof buf);
    if (buf[0]) {
        sonos_set_zone(buf);
        trigger_load_items();
    }
}

static void on_source_change(lv_event_t * e) {
    lv_obj_t * dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    if (sel == 0) {
        s_cur_source = SONOS_SRC_FAVORITES;
        s_cur_spot_user = 0;
    } else if (sel == 1) {
        s_cur_source = SONOS_SRC_PLAYLISTS;
        s_cur_spot_user = 0;
    } else {
        s_cur_source = SONOS_SRC_SPOTIFY;
        s_cur_spot_user = sel - 2;
    }
    if (s_lbl_fav_msg) {
        lv_label_set_text(s_lbl_fav_msg, tr("Laden...", "Loading..."));
        lv_obj_clear_flag(s_lbl_fav_msg, LV_OBJ_FLAG_HIDDEN);
    }
    trigger_load_items();
}

static void on_item_click(lv_event_t * e) {
    uint32_t now = lv_tick_get();
    if (now - s_last_item_click < 500) {
        return; /* Debounce accidental rapid double-tap */
    }
    s_last_item_click = now;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_item_count) return;
    const sonos_item_t * it = &s_items[idx];

    if (s_cur_source == SONOS_SRC_FAVORITES) {
        snprintf(s_pending_name, sizeof s_pending_name, "%s", it->name);
        s_pending_until = now + 4000;
        sonos_play_favorite(it->name);
        if (s_lbl_status) {
            lv_label_set_text_fmt(s_lbl_status, "▶ %s...", it->name);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        }
    } else {
        open_playlist_tracks_view(it->name, it->id[0] ? it->id : (it->uri[0] ? it->uri : it->name), s_cur_source);
    }
}

static void on_fav_refresh_click(lv_event_t * e) {
    (void)e;
    if (s_lbl_fav_msg) {
        lv_label_set_text(s_lbl_fav_msg, tr("Laden...", "Loading..."));
        lv_obj_clear_flag(s_lbl_fav_msg, LV_OBJ_FLAG_HIDDEN);
    }
    trigger_load_items();
}

static void populate_items_list(void) {
    if (!s_fav_list) return;
    lv_obj_clean(s_fav_list);

    if (s_item_count <= 0) {
        lv_obj_t * m = lv_label_create(s_fav_list);
        if (s_cur_source == SONOS_SRC_FAVORITES)
            lv_label_set_text(m, tr("Geen favorieten gevonden in Sonos.", "No favorites found in Sonos."));
        else if (s_cur_source == SONOS_SRC_PLAYLISTS)
            lv_label_set_text(m, tr("Geen Sonos-afspeellijsten gevonden.", "No Sonos playlists found."));
        else
            lv_label_set_text(m, tr("Geen Spotify-afspeellijsten gevonden.", "No Spotify playlists found."));
        lv_obj_set_style_text_color(m, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(m, SF(18), 0);
        return;
    }

    const char * sym = LV_SYMBOL_AUDIO;
    if (s_cur_source == SONOS_SRC_PLAYLISTS) sym = LV_SYMBOL_LIST;
    else if (s_cur_source == SONOS_SRC_SPOTIFY) sym = LV_SYMBOL_PLAY;

    for (int i = 0; i < s_item_count; i++) {
        lv_obj_t * btn = lv_btn_create(s_fav_list);
        lv_obj_set_size(btn, SX(450), SY(52));
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_ROW_BG), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_all(btn, SX(10), 0);
        lv_obj_add_event_cb(btn, on_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * ico = lv_label_create(btn);
        lv_label_set_text(ico, sym);
        lv_obj_set_style_text_color(ico, lv_color_hex(s_cur_source == SONOS_SRC_SPOTIFY ? 0x22cc66 : THEME_ACCENT_BLUE), 0);
        lv_obj_set_style_text_font(ico, SF(18), 0);
        lv_obj_align(ico, LV_ALIGN_LEFT_MID, SX(4), 0);

        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_items[i].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl, SF(18), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SX(390));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(34), 0);
    }
}

static void sonos_modal_tick_cb(lv_timer_t * t) {
    (void)t;
    if (!s_modal) return;

    /* Check if background items load completed */
    if (s_items_ready) {
        s_items_ready = 0;
        if (s_lbl_fav_msg) lv_obj_add_flag(s_lbl_fav_msg, LV_OBJ_FLAG_HIDDEN);
        populate_items_list();
    }
    if (s_tracks_ready) {
        s_tracks_ready = 0;
        populate_tracks_list();
    }
    if (s_search_ready) {
        s_search_ready = 0;
        populate_search_results();
    }

    sonos_state_t st;
    sonos_get_snapshot(&st);

    if (!st.connected) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, tr("⚠ Zoeken naar Sonos...", "⚠ Searching for Sonos..."));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xff4444), 0);
        }
        if (s_lbl_title) lv_label_set_text(s_lbl_title, tr("Controleer of speakers aanstaan", "Check if speakers are turned on"));
        if (s_lbl_artist) lv_label_set_text(s_lbl_artist, "");
        if (s_lbl_album) lv_label_set_text(s_lbl_album, "");
        if (s_art_box) lv_obj_add_flag(s_art_box, LV_OBJ_FLAG_HIDDEN);
        if (s_pos_container) lv_obj_add_flag(s_pos_container, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_title) { lv_obj_set_pos(s_lbl_title, 0, SY(36)); lv_obj_set_width(s_lbl_title, SX(436)); }
        if (s_lbl_artist) { lv_obj_set_pos(s_lbl_artist, 0, SY(96)); lv_obj_set_width(s_lbl_artist, SX(436)); }
        if (s_lbl_album) { lv_obj_set_pos(s_lbl_album, 0, SY(124)); lv_obj_set_width(s_lbl_album, SX(436)); }
        return;
    }

    /* Check pending playlist / favorite load status */
    if (s_pending_name[0]) {
        uint32_t now = lv_tick_get();
        if (now > s_pending_until || (st.playing && st.title[0])) {
            s_pending_name[0] = 0;
        }
    }

    /* Playback status */
    if (s_lbl_status) {
        if (s_pending_name[0]) {
            lv_label_set_text_fmt(s_lbl_status, "▶ %s...", s_pending_name);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        } else if (st.playing) {
            lv_label_set_text(s_lbl_status, tr("Speelt", "Playing"));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        } else {
            lv_label_set_text(s_lbl_status, tr("Gepauzeerd", "Paused"));
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xffaa22), 0);
        }
    }

    /* Artwork update */
    int art_ver = 0;
    const lv_img_dsc_t * art_dsc = sonos_get_art_big(&art_ver);
    if (art_ver != s_art_last_ver) {
        s_art_last_ver = art_ver;
        if (art_dsc && s_img_art && s_art_box) {
            lv_img_set_zoom(s_img_art, LV_IMG_ZOOM_NONE);
            lv_img_set_src(s_img_art, art_dsc);
            lv_obj_invalidate(s_img_art);
            lv_obj_clear_flag(s_art_box, LV_OBJ_FLAG_HIDDEN);

            if (s_lbl_title) {
                lv_obj_set_pos(s_lbl_title, SX(142), SY(36));
                lv_obj_set_width(s_lbl_title, SX(294));
            }
            if (s_lbl_artist) {
                lv_obj_set_pos(s_lbl_artist, SX(142), SY(96));
                lv_obj_set_width(s_lbl_artist, SX(294));
            }
            if (s_lbl_album) {
                lv_obj_set_pos(s_lbl_album, SX(142), SY(124));
                lv_obj_set_width(s_lbl_album, SX(294));
            }
        } else {
            if (s_art_box) lv_obj_add_flag(s_art_box, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_title) {
                lv_obj_set_pos(s_lbl_title, 0, SY(36));
                lv_obj_set_width(s_lbl_title, SX(436));
            }
            if (s_lbl_artist) {
                lv_obj_set_pos(s_lbl_artist, 0, SY(96));
                lv_obj_set_width(s_lbl_artist, SX(436));
            }
            if (s_lbl_album) {
                lv_obj_set_pos(s_lbl_album, 0, SY(124));
                lv_obj_set_width(s_lbl_album, SX(436));
            }
        }
    }

    /* Track metadata */
    if (s_lbl_title) {
        if (st.title[0]) lv_label_set_text(s_lbl_title, st.title);
        else if (s_pending_name[0]) lv_label_set_text(s_lbl_title, s_pending_name);
        else lv_label_set_text(s_lbl_title, st.playing ? tr("Geen titel", "No title") : tr("Geen muziek", "No music"));
    }
    if (s_lbl_artist) {
        lv_label_set_text(s_lbl_artist, st.artist[0] ? st.artist : "");
    }
    if (s_lbl_album) {
        lv_label_set_text(s_lbl_album, st.album[0] ? st.album : "");
    }

    /* Play/Pause button symbol */
    if (s_lbl_play) {
        lv_label_set_text(s_lbl_play, st.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    /* Track position slider */
    if (!st.is_radio && st.duration > 0) {
        if (s_pos_container) lv_obj_clear_flag(s_pos_container, LV_OBJ_FLAG_HIDDEN);
        if (s_slider_pos && !s_pos_dragging) {
            lv_slider_set_range(s_slider_pos, 0, st.duration);
            lv_slider_set_value(s_slider_pos, st.elapsed, LV_ANIM_OFF);
        }
        if (s_lbl_time && !s_pos_dragging) {
            char e_buf[16], d_buf[16];
            format_time(st.elapsed, e_buf, sizeof e_buf);
            format_time(st.duration, d_buf, sizeof d_buf);
            lv_label_set_text_fmt(s_lbl_time, "%s / %s", e_buf, d_buf);
        }
    } else {
        if (s_pos_container) lv_obj_add_flag(s_pos_container, LV_OBJ_FLAG_HIDDEN);
    }

    /* Volume label */
    if (s_lbl_vol) {
        lv_label_set_text_fmt(s_lbl_vol, "%s: %d%%", tr("Volume", "Volume"), st.volume);
    }
}

/* =========================================================================
 * Playlist Tracks View
 * ========================================================================= */
static lv_obj_t * s_tracks_view = NULL;
static lv_obj_t * s_tracks_list = NULL;
static lv_obj_t * s_lbl_tracks_subtitle = NULL;
static sonos_track_t s_tracks[SONOS_MAX_TRACKS];
static int           s_track_count = 0;
static volatile int  s_tracks_loading = 0;
static char          s_cur_playlist_name[64] = "";
static char          s_cur_playlist_id[128] = "";
static sonos_source_t s_tracks_source = SONOS_SRC_PLAYLISTS;

static void * tracks_loader_thread(void * arg) {
    (void)arg;
    int n = sonos_get_playlist_tracks(s_tracks_source, s_cur_playlist_id, s_tracks, SONOS_MAX_TRACKS);
    s_track_count = n;
    s_tracks_ready = 1;
    s_tracks_loading = 0;
    return NULL;
}

static void trigger_load_tracks(void) {
    if (s_tracks_loading) return;
    s_tracks_loading = 1;
    s_tracks_ready = 0;
    pthread_t tid;
    if (pthread_create(&tid, NULL, tracks_loader_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        s_tracks_loading = 0;
    }
}

static void close_playlist_tracks_view(void) {
    if (s_tracks_view) {
        lv_obj_del(s_tracks_view);
        s_tracks_view = NULL;
        s_tracks_list = NULL;
        s_lbl_tracks_subtitle = NULL;
    }
}

static void on_tracks_back_click(lv_event_t * e) {
    (void)e;
    close_playlist_tracks_view();
}

static void on_track_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_track_count) return;
    const sonos_track_t * trk = &s_tracks[idx];

    snprintf(s_pending_name, sizeof s_pending_name, "%s", trk->title);
    s_pending_until = lv_tick_get() + 4000;

    if (s_tracks_source == SONOS_SRC_PLAYLISTS) {
        sonos_play_playlist_track(s_cur_playlist_id, trk->track_nr);
    } else {
        sonos_play_spotify_playlist_track(s_cur_playlist_id, trk->track_nr, trk->uri);
    }

    if (s_lbl_status) {
        lv_label_set_text_fmt(s_lbl_status, "▶ %s...", trk->title);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
    }
}

static void on_tracks_play_all_click(lv_event_t * e) {
    (void)e;
    snprintf(s_pending_name, sizeof s_pending_name, "%s", s_cur_playlist_name);
    s_pending_until = lv_tick_get() + 4000;

    if (s_tracks_source == SONOS_SRC_PLAYLISTS) {
        sonos_play_playlist_track(s_cur_playlist_id, 1);
    } else {
        sonos_play_spotify_playlist_track(s_cur_playlist_id, 1, "");
    }

    if (s_lbl_status) {
        lv_label_set_text_fmt(s_lbl_status, "▶ %s...", s_cur_playlist_name);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
    }
}

static void populate_tracks_list(void) {
    if (!s_tracks_list || !s_tracks_view) return;
    lv_obj_clean(s_tracks_list);

    if (s_lbl_tracks_subtitle) {
        if (s_track_count <= 0) {
            lv_label_set_text(s_lbl_tracks_subtitle, tr("Geen nummers gevonden in deze afspeellijst", "No tracks found in this playlist"));
        } else {
            lv_label_set_text_fmt(s_lbl_tracks_subtitle, "%d %s", s_track_count, tr("nummers (tik om af te spelen)", "tracks (tap to play)"));
        }
    }

    for (int i = 0; i < s_track_count; i++) {
        lv_obj_t * btn = lv_btn_create(s_tracks_list);
        lv_obj_set_size(btn, SX(450), SY(52));
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_ROW_BG), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_all(btn, SX(8), 0);
        lv_obj_add_event_cb(btn, on_track_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Track number badge */
        lv_obj_t * nr_lbl = lv_label_create(btn);
        lv_label_set_text_fmt(nr_lbl, "%d.", s_tracks[i].track_nr);
        lv_obj_set_style_text_color(nr_lbl, lv_color_hex(THEME_ACCENT_BLUE), 0);
        lv_obj_set_style_text_font(nr_lbl, SF(15), 0);
        lv_obj_align(nr_lbl, LV_ALIGN_LEFT_MID, SX(2), 0);

        /* Title & Artist label */
        lv_obj_t * lbl = lv_label_create(btn);
        if (s_tracks[i].artist[0]) {
            lv_label_set_text_fmt(lbl, "%s  —  %s", s_tracks[i].title, s_tracks[i].artist);
        } else {
            lv_label_set_text(lbl, s_tracks[i].title);
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl, SF(15), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SX(340));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(32), 0);

        /* Duration */
        if (s_tracks[i].duration > 0) {
            lv_obj_t * dur_lbl = lv_label_create(btn);
            char d_buf[16];
            format_time(s_tracks[i].duration, d_buf, sizeof d_buf);
            lv_label_set_text(dur_lbl, d_buf);
            lv_obj_set_style_text_color(dur_lbl, lv_color_hex(THEME_TEXT_DIM), 0);
            lv_obj_set_style_text_font(dur_lbl, SF(13), 0);
            lv_obj_align(dur_lbl, LV_ALIGN_RIGHT_MID, SX(-4), 0);
        }
    }
}

static void open_playlist_tracks_view(const char * name, const char * id_or_uri, sonos_source_t src) {
    if (!s_modal) return;
    close_playlist_tracks_view();

    snprintf(s_cur_playlist_name, sizeof s_cur_playlist_name, "%s", name ? name : "");
    snprintf(s_cur_playlist_id, sizeof s_cur_playlist_id, "%s", id_or_uri ? id_or_uri : "");
    s_tracks_source = src;
    s_track_count = 0;

    s_tracks_view = lv_obj_create(s_modal);
    lv_obj_set_size(s_tracks_view, SX(490), SY(510));
    lv_obj_set_pos(s_tracks_view, SX(514), SY(74));
    lv_obj_set_style_bg_color(s_tracks_view, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(s_tracks_view, 16, 0);
    lv_obj_set_style_border_color(s_tracks_view, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_tracks_view, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_pad_all(s_tracks_view, SX(14), 0);
    lv_obj_clear_flag(s_tracks_view, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t * btn_back = lv_btn_create(s_tracks_view);
    lv_obj_set_size(btn_back, SX(80), SY(44));
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, on_tracks_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bkl = lv_label_create(btn_back);
    lv_label_set_text_fmt(bkl, "%s %s", LV_SYMBOL_LEFT, tr("Terug", "Back"));
    lv_obj_set_style_text_color(bkl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bkl, SF(15), 0);
    lv_obj_center(bkl);

    /* Playlist title label */
    lv_obj_t * lbl_title = lv_label_create(s_tracks_view);
    lv_label_set_text(lbl_title, s_cur_playlist_name);
    lv_obj_set_style_text_font(lbl_title, SF(18), 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_title, SX(240));
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, SX(90), SY(10));

    /* Play all button */
    lv_obj_t * btn_all = lv_btn_create(s_tracks_view);
    lv_obj_set_size(btn_all, SX(110), SY(44));
    lv_obj_align(btn_all, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_all, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x2a5d96), 0);
    lv_obj_set_style_radius(btn_all, 8, 0);
    lv_obj_add_event_cb(btn_all, on_tracks_play_all_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * all_lbl = lv_label_create(btn_all);
    lv_label_set_text_fmt(all_lbl, "%s %s", LV_SYMBOL_PLAY, tr("Alles", "Play all"));
    lv_obj_set_style_text_color(all_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(all_lbl, SF(15), 0);
    lv_obj_center(all_lbl);

    /* Subtitle */
    s_lbl_tracks_subtitle = lv_label_create(s_tracks_view);
    lv_obj_set_style_text_font(s_lbl_tracks_subtitle, SF(14), 0);
    lv_obj_set_style_text_color(s_lbl_tracks_subtitle, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_tracks_subtitle, tr("Nummers ophalen...", "Fetching tracks..."));
    lv_obj_align(s_lbl_tracks_subtitle, LV_ALIGN_TOP_LEFT, SX(6), SY(52));

    /* Tracks list container */
    s_tracks_list = lv_obj_create(s_tracks_view);
    lv_obj_set_size(s_tracks_list, SX(466), SY(418));
    lv_obj_set_pos(s_tracks_list, 0, SY(74));
    lv_obj_set_style_bg_opa(s_tracks_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tracks_list, 0, 0);
    lv_obj_set_style_pad_all(s_tracks_list, 0, 0);
    lv_obj_set_style_pad_row(s_tracks_list, 6, 0);
    lv_obj_add_flag(s_tracks_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_tracks_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_tracks_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_tracks_list, LV_FLEX_FLOW_COLUMN);

    trigger_load_tracks();
}

/* =========================================================================
 * Spotify Music Search View
 * ========================================================================= */
static lv_obj_t * s_search_modal = NULL;
static lv_obj_t * s_ta_search = NULL;
static lv_obj_t * s_kb_search = NULL;
static lv_obj_t * s_search_list = NULL;
static lv_obj_t * s_btn_cat_tracks = NULL;
static lv_obj_t * s_btn_cat_albums = NULL;
static lv_obj_t * s_btn_cat_playlists = NULL;
static lv_obj_t * s_lbl_search_msg = NULL;
static sonos_search_result_t s_search_res = {0};
static sonos_search_type_t   s_search_cat = SONOS_SEARCH_TRACKS;
static volatile int          s_search_loading = 0;
static char                  s_last_query[128] = "";

static void close_search_view(void) {
    if (s_search_modal) {
        lv_obj_del(s_search_modal);
        s_search_modal = NULL;
        s_ta_search = NULL;
        s_kb_search = NULL;
        s_search_list = NULL;
        s_btn_cat_tracks = NULL;
        s_btn_cat_albums = NULL;
        s_btn_cat_playlists = NULL;
        s_lbl_search_msg = NULL;
    }
}

static void on_search_close_click(lv_event_t * e) {
    (void)e;
    close_search_view();
}

static void * search_loader_thread(void * arg) {
    (void)arg;
    sonos_spotify_search(s_last_query, &s_search_res);
    s_search_ready = 1;
    s_search_loading = 0;
    return NULL;
}

static void trigger_spotify_search(const char * q) {
    if (!q || !q[0] || s_search_loading) return;
    snprintf(s_last_query, sizeof s_last_query, "%s", q);
    s_search_loading = 1;
    s_search_ready = 0;
    if (s_lbl_search_msg) {
        lv_label_set_text_fmt(s_lbl_search_msg, "%s '%s'...", tr("Zoeken naar", "Searching for"), q);
    }
    if (s_kb_search) {
        lv_obj_add_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_search_list) {
        lv_obj_set_height(s_search_list, SY(440));
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, search_loader_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        s_search_loading = 0;
    }
}

static void on_search_go_click(lv_event_t * e) {
    (void)e;
    if (!s_ta_search) return;
    const char * txt = lv_textarea_get_text(s_ta_search);
    if (txt && txt[0]) trigger_spotify_search(txt);
}

static void on_search_kb_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_search_go_click(NULL);
    } else if (code == LV_EVENT_CANCEL) {
        if (s_kb_search) lv_obj_add_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
        if (s_search_list) lv_obj_set_height(s_search_list, SY(440));
    }
}

static void on_ta_search_click(lv_event_t * e) {
    (void)e;
    if (s_kb_search) {
        lv_obj_clear_flag(s_kb_search, LV_OBJ_FLAG_HIDDEN);
        if (s_search_list) lv_obj_set_height(s_search_list, SY(220));
    }
}

static void update_cat_buttons_style(void) {
    if (s_btn_cat_tracks)
        lv_obj_set_style_bg_color(s_btn_cat_tracks, lv_color_hex(s_search_cat == SONOS_SEARCH_TRACKS ? THEME_ACCENT_BLUE : THEME_BTN_BG), 0);
    if (s_btn_cat_albums)
        lv_obj_set_style_bg_color(s_btn_cat_albums, lv_color_hex(s_search_cat == SONOS_SEARCH_ALBUMS ? THEME_ACCENT_BLUE : THEME_BTN_BG), 0);
    if (s_btn_cat_playlists)
        lv_obj_set_style_bg_color(s_btn_cat_playlists, lv_color_hex(s_search_cat == SONOS_SEARCH_PLAYLISTS ? THEME_ACCENT_BLUE : THEME_BTN_BG), 0);
}

static void on_cat_click(lv_event_t * e) {
    int cat = (int)(intptr_t)lv_event_get_user_data(e);
    s_search_cat = (sonos_search_type_t)cat;
    update_cat_buttons_style();
    populate_search_results();
}

static void on_search_result_item_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_search_cat == SONOS_SEARCH_TRACKS) {
        if (idx < 0 || idx >= s_search_res.track_count) return;
        const sonos_search_item_t * item = &s_search_res.tracks[idx];
        snprintf(s_pending_name, sizeof s_pending_name, "%s", item->name);
        s_pending_until = lv_tick_get() + 4000;
        sonos_play_spotify(item->uri);
        if (s_lbl_status) {
            lv_label_set_text_fmt(s_lbl_status, "▶ %s...", item->name);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        }
        close_search_view();
    } else if (s_search_cat == SONOS_SEARCH_ALBUMS) {
        if (idx < 0 || idx >= s_search_res.album_count) return;
        const sonos_search_item_t * item = &s_search_res.albums[idx];
        snprintf(s_pending_name, sizeof s_pending_name, "%s", item->name);
        s_pending_until = lv_tick_get() + 4000;
        sonos_play_spotify(item->uri);
        if (s_lbl_status) {
            lv_label_set_text_fmt(s_lbl_status, "▶ %s...", item->name);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x22cc66), 0);
        }
        close_search_view();
    } else if (s_search_cat == SONOS_SEARCH_PLAYLISTS) {
        if (idx < 0 || idx >= s_search_res.playlist_count) return;
        const sonos_search_item_t * item = &s_search_res.playlists[idx];
        char pl_name[96], pl_uri[128];
        snprintf(pl_name, sizeof pl_name, "%s", item->name);
        snprintf(pl_uri, sizeof pl_uri, "%s", item->uri);
        close_search_view();
        open_playlist_tracks_view(pl_name, pl_uri, SONOS_SRC_SPOTIFY);
    }
}

static void populate_search_results(void) {
    if (!s_search_list || !s_search_modal) return;
    lv_obj_clean(s_search_list);

    if (s_btn_cat_tracks) {
        lv_obj_t * l = lv_obj_get_child(s_btn_cat_tracks, 0);
        if (l) lv_label_set_text_fmt(l, "%s (%d)", tr("Nummers", "Tracks"), s_search_res.track_count);
    }
    if (s_btn_cat_albums) {
        lv_obj_t * l = lv_obj_get_child(s_btn_cat_albums, 0);
        if (l) lv_label_set_text_fmt(l, "%s (%d)", tr("Albums", "Albums"), s_search_res.album_count);
    }
    if (s_btn_cat_playlists) {
        lv_obj_t * l = lv_obj_get_child(s_btn_cat_playlists, 0);
        if (l) lv_label_set_text_fmt(l, "%s (%d)", tr("Playlists", "Playlists"), s_search_res.playlist_count);
    }

    int count = 0;
    if (s_search_cat == SONOS_SEARCH_TRACKS) count = s_search_res.track_count;
    else if (s_search_cat == SONOS_SEARCH_ALBUMS) count = s_search_res.album_count;
    else count = s_search_res.playlist_count;

    if (s_lbl_search_msg) {
        if (count == 0) {
            lv_label_set_text(s_lbl_search_msg, tr("Geen resultaten gevonden in deze categorie", "No results found in this category"));
        } else {
            lv_label_set_text_fmt(s_lbl_search_msg, "%d %s", count, tr("resultaten gevonden", "results found"));
        }
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t * btn = lv_btn_create(s_search_list);
        lv_obj_set_size(btn, SX(940), SY(52));
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_ROW_BG), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_all(btn, SX(10), 0);
        lv_obj_add_event_cb(btn, on_search_result_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        const char * sym = LV_SYMBOL_PLAY;
        const char * name = "";
        const char * artist = "";
        int dur = 0;

        if (s_search_cat == SONOS_SEARCH_TRACKS) {
            sym = LV_SYMBOL_PLAY;
            name = s_search_res.tracks[i].name;
            artist = s_search_res.tracks[i].artist;
            dur = s_search_res.tracks[i].duration;
        } else if (s_search_cat == SONOS_SEARCH_ALBUMS) {
            sym = LV_SYMBOL_AUDIO;
            name = s_search_res.albums[i].name;
            artist = s_search_res.albums[i].artist;
        } else {
            sym = LV_SYMBOL_LIST;
            name = s_search_res.playlists[i].name;
            artist = s_search_res.playlists[i].artist;
        }

        lv_obj_t * ico = lv_label_create(btn);
        lv_label_set_text(ico, sym);
        lv_obj_set_style_text_color(ico, lv_color_hex(0x22cc66), 0);
        lv_obj_set_style_text_font(ico, SF(18), 0);
        lv_obj_align(ico, LV_ALIGN_LEFT_MID, SX(4), 0);

        lv_obj_t * lbl = lv_label_create(btn);
        if (artist && artist[0]) {
            lv_label_set_text_fmt(lbl, "%s  —  %s", name, artist);
        } else {
            lv_label_set_text(lbl, name);
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl, SF(16), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SX(780));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(36), 0);

        if (dur > 0) {
            lv_obj_t * dur_lbl = lv_label_create(btn);
            char d_buf[16];
            format_time(dur, d_buf, sizeof d_buf);
            lv_label_set_text(dur_lbl, d_buf);
            lv_obj_set_style_text_color(dur_lbl, lv_color_hex(THEME_TEXT_DIM), 0);
            lv_obj_set_style_text_font(dur_lbl, SF(14), 0);
            lv_obj_align(dur_lbl, LV_ALIGN_RIGHT_MID, SX(-4), 0);
        }
    }
}

static void open_search_view(void) {
    if (s_search_modal) return;

    s_search_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_search_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_search_modal, 0, 0);
    lv_obj_set_style_bg_color(s_search_modal, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(s_search_modal, 0, 0);
    lv_obj_set_style_radius(s_search_modal, 0, 0);
    lv_obj_set_style_pad_all(s_search_modal, 0, 0);
    lv_obj_clear_flag(s_search_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    int cb_w = (DISP_HOR < 1000) ? 68 : 76;
    int cb_h = (DISP_VER < 600) ? 46 : 52;
    lv_obj_t * b_close = lv_btn_create(s_search_modal);
    lv_obj_set_size(b_close, cb_w, cb_h);
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(b_close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_close, 10, 0);
    lv_obj_add_event_cb(b_close, on_search_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(b_close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cl, SF(22), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(cl);

    lv_obj_t * title = lv_label_create(s_search_modal);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(24), 0);
    lv_label_set_text_fmt(title, "%s Spotify", tr("Zoeken in", "Search in"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(20), SY(14));

    /* Search input row */
    s_ta_search = lv_textarea_create(s_search_modal);
    lv_obj_set_size(s_ta_search, SX(560), SY(46));
    lv_obj_set_pos(s_ta_search, SX(20), SY(60));
    lv_textarea_set_placeholder_text(s_ta_search, tr("Typ artiest, nummer of album...", "Type artist, song or album..."));
    lv_textarea_set_one_line(s_ta_search, true);
    lv_obj_set_style_text_font(s_ta_search, SF(16), 0);
    lv_obj_set_style_bg_color(s_ta_search, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_text_color(s_ta_search, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_border_color(s_ta_search, lv_color_hex(THEME_BORDER), 0);
    lv_obj_add_event_cb(s_ta_search, on_ta_search_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_go = lv_btn_create(s_search_modal);
    lv_obj_set_size(btn_go, SX(110), SY(46));
    lv_obj_set_pos(btn_go, SX(590), SY(60));
    lv_obj_set_style_bg_color(btn_go, lv_color_hex(0x22cc66), 0);
    lv_obj_set_style_radius(btn_go, 10, 0);
    lv_obj_add_event_cb(btn_go, on_search_go_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gol = lv_label_create(btn_go);
    lv_label_set_text(gol, tr("Zoek", "Search"));
    lv_obj_set_style_text_color(gol, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(gol, SF(16), 0);
    lv_obj_center(gol);

    /* Category Buttons row */
    int cat_w = SX(125);
    int cat_h = SY(38);
    int cat_y = SY(116);
    int cat_x = SX(20);

    s_btn_cat_tracks = lv_btn_create(s_search_modal);
    lv_obj_set_size(s_btn_cat_tracks, cat_w, cat_h);
    lv_obj_set_pos(s_btn_cat_tracks, cat_x, cat_y);
    lv_obj_set_style_radius(s_btn_cat_tracks, 8, 0);
    lv_obj_add_event_cb(s_btn_cat_tracks, on_cat_click, LV_EVENT_CLICKED, (void *)(intptr_t)SONOS_SEARCH_TRACKS);
    lv_obj_t * ctl1 = lv_label_create(s_btn_cat_tracks);
    lv_label_set_text_fmt(ctl1, "%s (%d)", tr("Nummers", "Tracks"), 0);
    lv_obj_set_style_text_font(ctl1, SF(14), 0);
    lv_obj_center(ctl1);
    cat_x += cat_w + SX(10);

    s_btn_cat_albums = lv_btn_create(s_search_modal);
    lv_obj_set_size(s_btn_cat_albums, cat_w, cat_h);
    lv_obj_set_pos(s_btn_cat_albums, cat_x, cat_y);
    lv_obj_set_style_radius(s_btn_cat_albums, 8, 0);
    lv_obj_add_event_cb(s_btn_cat_albums, on_cat_click, LV_EVENT_CLICKED, (void *)(intptr_t)SONOS_SEARCH_ALBUMS);
    lv_obj_t * ctl2 = lv_label_create(s_btn_cat_albums);
    lv_label_set_text_fmt(ctl2, "%s (%d)", tr("Albums", "Albums"), 0);
    lv_obj_set_style_text_font(ctl2, SF(14), 0);
    lv_obj_center(ctl2);
    cat_x += cat_w + SX(10);

    s_btn_cat_playlists = lv_btn_create(s_search_modal);
    lv_obj_set_size(s_btn_cat_playlists, cat_w, cat_h);
    lv_obj_set_pos(s_btn_cat_playlists, cat_x, cat_y);
    lv_obj_set_style_radius(s_btn_cat_playlists, 8, 0);
    lv_obj_add_event_cb(s_btn_cat_playlists, on_cat_click, LV_EVENT_CLICKED, (void *)(intptr_t)SONOS_SEARCH_PLAYLISTS);
    lv_obj_t * ctl3 = lv_label_create(s_btn_cat_playlists);
    lv_label_set_text_fmt(ctl3, "%s (%d)", tr("Playlists", "Playlists"), 0);
    lv_obj_set_style_text_font(ctl3, SF(14), 0);
    lv_obj_center(ctl3);
    cat_x += cat_w + SX(16);

    s_lbl_search_msg = lv_label_create(s_search_modal);
    lv_obj_set_style_text_font(s_lbl_search_msg, SF(14), 0);
    lv_obj_set_style_text_color(s_lbl_search_msg, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_search_msg, tr("Typ een zoekterm en druk op Zoek", "Type a search term and press Search"));
    lv_obj_set_pos(s_lbl_search_msg, cat_x, cat_y + SY(8));

    update_cat_buttons_style();

    /* Results list */
    s_search_list = lv_obj_create(s_search_modal);
    lv_obj_set_size(s_search_list, SX(984), SY(220));
    lv_obj_set_pos(s_search_list, SX(20), SY(164));
    lv_obj_set_style_bg_opa(s_search_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_search_list, 0, 0);
    lv_obj_set_style_pad_all(s_search_list, 0, 0);
    lv_obj_set_style_pad_row(s_search_list, 6, 0);
    lv_obj_add_flag(s_search_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_search_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_search_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_search_list, LV_FLEX_FLOW_COLUMN);

    /* Virtual Keyboard */
    s_kb_search = lv_keyboard_create(s_search_modal);
    lv_obj_set_size(s_kb_search, LV_HOR_RES, SY(210));
    lv_obj_align(s_kb_search, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb_search, s_ta_search);
    lv_obj_add_event_cb(s_kb_search, on_search_kb_event, LV_EVENT_ALL, NULL);
}

static void on_search_click(lv_event_t * e) {
    (void)e;
    open_search_view();
}

lv_obj_t * screen_create(void) {
    if (s_modal) lv_obj_del(s_modal);

    s_art_last_ver = -1;
    sonos_start();
    sonos_get_spotify_config(&s_spot_cfg);

    s_modal = lv_obj_create(NULL);
    lv_obj_set_size(s_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Top Header --- */
    lv_obj_t * title = lv_label_create(s_modal);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Sonos");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(16));

    /* Room selector dropdown */
    char rooms[SONOS_MAX_ZONES][48];
    int n_rooms = sonos_probe(rooms, SONOS_MAX_ZONES);
    char opts[1024] = "";
    int sel_idx = 0;
    if (n_rooms > 0) {
        for (int i = 0; i < n_rooms; i++) {
            if (opts[0]) strncat(opts, "\n", sizeof opts - strlen(opts) - 1);
            strncat(opts, rooms[i], sizeof opts - strlen(opts) - 1);
            if (settings.sonos_zone[0] && strcmp(settings.sonos_zone, rooms[i]) == 0)
                sel_idx = i;
        }
    } else if (settings.sonos_zone[0]) {
        snprintf(opts, sizeof opts, "%s", settings.sonos_zone);
    } else {
        snprintf(opts, sizeof opts, "%s", tr("Kamer kiezen...", "Select room..."));
    }

    s_dd_zone = lv_dropdown_create(s_modal);
    lv_obj_set_size(s_dd_zone, SX(260), SY(48));
    lv_obj_align(s_dd_zone, LV_ALIGN_TOP_LEFT, SX(150), SY(12));
    lv_obj_set_style_text_font(s_dd_zone, SF(18), 0);
    lv_dropdown_set_options(s_dd_zone, opts);
    lv_dropdown_set_selected(s_dd_zone, sel_idx);
    lv_obj_add_event_cb(s_dd_zone, on_zone_change, LV_EVENT_VALUE_CHANGED, NULL);

    /* Close button */
    int cb_w = (DISP_HOR < 1000) ? 68 : 76;
    int cb_h = (DISP_VER < 600) ? 46 : 52;
    lv_obj_t * b_close = lv_btn_create(s_modal);
    lv_obj_set_size(b_close, cb_w, cb_h);
    lv_obj_align(b_close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(b_close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_close, 10, 0);
    lv_obj_set_ext_click_area(b_close, 30);
    lv_obj_add_event_cb(b_close, on_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(b_close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cl, SF(22), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(cl);

    /* Settings button (matching size, to the left of the close button) */
    lv_obj_t * b_set = lv_btn_create(s_modal);
    lv_obj_set_size(b_set, cb_w, cb_h);
    lv_obj_align(b_set, LV_ALIGN_TOP_RIGHT, SX(-12) - cb_w - SX(10), SY(8));
    lv_obj_set_style_bg_color(b_set, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_set, 10, 0);
    lv_obj_set_ext_click_area(b_set, 25);
    lv_obj_add_event_cb(b_set, on_settings_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(b_set);
    lv_label_set_text(sl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(sl, SF(22), 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(sl);

    /* Search button (if Spotify configured) */
    if (s_spot_cfg.configured || s_spot_cfg.user_count > 0) {
        lv_obj_t * b_search = lv_btn_create(s_modal);
        lv_obj_set_size(b_search, cb_w, cb_h);
        lv_obj_align(b_search, LV_ALIGN_TOP_RIGHT, SX(-12) - 2 * (cb_w + SX(10)), SY(8));
        lv_obj_set_style_bg_color(b_search, lv_color_hex(THEME_BTN_BG), 0);
        lv_obj_set_style_radius(b_search, 10, 0);
        lv_obj_set_ext_click_area(b_search, 25);
        lv_obj_add_event_cb(b_search, on_search_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t * sch = lv_label_create(b_search);
        lv_label_set_text(sch, tr("Zoek", "Search"));
        lv_obj_set_style_text_font(sch, SF(17), 0);
        lv_obj_set_style_text_color(sch, lv_color_hex(0x22cc66), 0);
        lv_obj_center(sch);
    }

    /* --- Left Card: Now Playing --- */
    lv_obj_t * card_left = lv_obj_create(s_modal);
    lv_obj_set_size(card_left, SX(476), SY(510));
    lv_obj_set_pos(card_left, SX(20), SY(74));
    lv_obj_set_style_bg_color(card_left, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(card_left, 16, 0);
    lv_obj_set_style_border_color(card_left, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(card_left, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_pad_all(card_left, SX(20), 0);
    lv_obj_clear_flag(card_left, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_status = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_status, SF(18), 0);
    lv_label_set_text(s_lbl_status, tr("Verbinden...", "Connecting..."));
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 0, SY(4));

    /* Artwork box (130x130, rounded corners) */
    s_art_box = lv_obj_create(card_left);
    lv_obj_set_size(s_art_box, SUNI(130), SUNI(130));
    lv_obj_set_pos(s_art_box, 0, SY(36));
    lv_obj_set_style_radius(s_art_box, 12, 0);
    lv_obj_set_style_clip_corner(s_art_box, true, 0);
    lv_obj_set_style_bg_color(s_art_box, lv_color_hex(settings.home_light_mode ? 0xdae2ec : 0x0a1420), 0);
    lv_obj_set_style_border_width(s_art_box, 0, 0);
    lv_obj_set_style_pad_all(s_art_box, 0, 0);
    lv_obj_clear_flag(s_art_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_art_box, LV_OBJ_FLAG_HIDDEN);

    s_img_art = lv_img_create(s_art_box);
    lv_img_set_pivot(s_img_art, 0, 0);
    lv_obj_set_pos(s_img_art, 0, 0);

    /* Metadata labels */
    s_lbl_title = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_title, SF(22), 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_width(s_lbl_title, SX(436));
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lbl_title, "...");
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 0, SY(36));

    s_lbl_artist = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_artist, SF(18), 0);
    lv_obj_set_style_text_color(s_lbl_artist, lv_color_hex(THEME_ACCENT_BLUE), 0);
    lv_obj_set_width(s_lbl_artist, SX(436));
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_artist, "");
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_LEFT, 0, SY(96));

    s_lbl_album = lv_label_create(card_left);
    lv_obj_set_style_text_font(s_lbl_album, SF(16), 0);
    lv_obj_set_style_text_color(s_lbl_album, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_width(s_lbl_album, SX(436));
    lv_label_set_long_mode(s_lbl_album, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_album, "");
    lv_obj_align(s_lbl_album, LV_ALIGN_TOP_LEFT, 0, SY(124));

    /* Transport Buttons Row */
    lv_obj_t * tbar = lv_obj_create(card_left);
    lv_obj_set_size(tbar, SX(436), SY(70));
    lv_obj_align(tbar, LV_ALIGN_TOP_MID, 0, SY(184));
    lv_obj_set_style_bg_opa(tbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    lv_obj_set_style_pad_all(tbar, 0, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * btn_prev = lv_btn_create(tbar);
    lv_obj_set_size(btn_prev, SX(80), SY(56));
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_prev, 12, 0);
    lv_obj_add_event_cb(btn_prev, on_prev_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * pl = lv_label_create(btn_prev);
    lv_label_set_text(pl, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(pl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(pl, SF(22), 0);
    lv_obj_center(pl);

    s_btn_play = lv_btn_create(tbar);
    lv_obj_set_size(s_btn_play, SX(110), SY(60));
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x2a5d96), 0);
    lv_obj_set_style_radius(s_btn_play, 14, 0);
    lv_obj_add_event_cb(s_btn_play, on_play_pause_click, LV_EVENT_CLICKED, NULL);
    s_lbl_play = lv_label_create(s_btn_play);
    lv_label_set_text(s_lbl_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_play, SF(26), 0);
    lv_obj_center(s_lbl_play);

    lv_obj_t * btn_next = lv_btn_create(tbar);
    lv_obj_set_size(btn_next, SX(80), SY(56));
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_next, 12, 0);
    lv_obj_add_event_cb(btn_next, on_next_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = lv_label_create(btn_next);
    lv_label_set_text(nl, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(nl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(nl, SF(22), 0);
    lv_obj_center(nl);

    /* Track Position Section (hidden when radio stream) */
    s_pos_container = lv_obj_create(card_left);
    lv_obj_set_size(s_pos_container, SX(436), SY(68));
    lv_obj_align(s_pos_container, LV_ALIGN_TOP_MID, 0, SY(268));
    lv_obj_set_style_bg_opa(s_pos_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pos_container, 0, 0);
    lv_obj_set_style_pad_all(s_pos_container, 0, 0);
    lv_obj_clear_flag(s_pos_container, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_time = lv_label_create(s_pos_container);
    lv_obj_set_style_text_font(s_lbl_time, SF(15), 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_time, "00:00 / 00:00");
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_LEFT, SX(4), SY(2));

    s_slider_pos = lv_slider_create(s_pos_container);
    lv_obj_set_size(s_slider_pos, SX(416), SY(16));
    lv_obj_align(s_slider_pos, LV_ALIGN_TOP_MID, 0, SY(32));
    lv_slider_set_range(s_slider_pos, 0, 100);
    lv_obj_set_style_bg_color(s_slider_pos, lv_color_hex(THEME_ROW_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_pos, lv_color_hex(THEME_ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_pos, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_pos, on_pos_slider_event, LV_EVENT_ALL, NULL);

    /* Volume Row ([-] Volume: XX% [+]) */
    lv_obj_t * vol_row = lv_obj_create(card_left);
    lv_obj_set_size(vol_row, SX(436), SY(56));
    lv_obj_align(vol_row, LV_ALIGN_TOP_MID, 0, SY(376));
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * btn_vdn = lv_btn_create(vol_row);
    lv_obj_set_size(btn_vdn, SX(72), SY(48));
    lv_obj_set_style_bg_color(btn_vdn, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_vdn, 10, 0);
    lv_obj_add_event_cb(btn_vdn, on_vol_down_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * vdl = lv_label_create(btn_vdn);
    lv_label_set_text(vdl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(vdl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(vdl, SF(20), 0);
    lv_obj_center(vdl);

    s_lbl_vol = lv_label_create(vol_row);
    lv_obj_set_style_text_font(s_lbl_vol, SF(18), 0);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(s_lbl_vol, tr("Volume: 0%", "Volume: 0%"));

    lv_obj_t * btn_vup = lv_btn_create(vol_row);
    lv_obj_set_size(btn_vup, SX(72), SY(48));
    lv_obj_set_style_bg_color(btn_vup, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_vup, 10, 0);
    lv_obj_add_event_cb(btn_vup, on_vol_up_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * vul = lv_label_create(btn_vup);
    lv_label_set_text(vul, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(vul, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(vul, SF(20), 0);
    lv_obj_center(vul);

    /* --- Right Card: Favorites & Radio --- */
    lv_obj_t * card_right = lv_obj_create(s_modal);
    lv_obj_set_size(card_right, SX(490), SY(510));
    lv_obj_set_pos(card_right, SX(514), SY(74));
    lv_obj_set_style_bg_color(card_right, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(card_right, 16, 0);
    lv_obj_set_style_border_color(card_right, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(card_right, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_pad_all(card_right, SX(16), 0);
    lv_obj_clear_flag(card_right, LV_OBJ_FLAG_SCROLLABLE);

    /* Source dropdown */
    char src_opts[512] = "";
    snprintf(src_opts, sizeof src_opts, "%s\n%s",
             tr("Favorieten & Radio", "Favorites & Radio"),
             tr("Sonos Playlists", "Sonos Playlists"));
    if (s_spot_cfg.configured && s_spot_cfg.user_count > 0) {
        for (int i = 0; i < s_spot_cfg.user_count; i++) {
            char uline[64];
            snprintf(uline, sizeof uline, "\nSpotify: %s", s_spot_cfg.user_names[i]);
            strncat(src_opts, uline, sizeof src_opts - strlen(src_opts) - 1);
        }
    }

    s_dd_source = lv_dropdown_create(card_right);
    lv_obj_set_size(s_dd_source, SX(390), SY(46));
    lv_obj_align(s_dd_source, LV_ALIGN_TOP_LEFT, SX(4), SY(2));
    lv_obj_set_style_text_font(s_dd_source, SF(18), 0);
    lv_dropdown_set_options(s_dd_source, src_opts);
    int init_src_idx = 0;
    if (s_cur_source == SONOS_SRC_FAVORITES) init_src_idx = 0;
    else if (s_cur_source == SONOS_SRC_PLAYLISTS) init_src_idx = 1;
    else init_src_idx = 2 + s_cur_spot_user;
    lv_dropdown_set_selected(s_dd_source, init_src_idx);
    lv_obj_add_event_cb(s_dd_source, on_source_change, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * b_ref = lv_btn_create(card_right);
    lv_obj_set_size(b_ref, SX(50), SY(46));
    lv_obj_align(b_ref, LV_ALIGN_TOP_RIGHT, SX(-4), SY(2));
    lv_obj_set_style_bg_color(b_ref, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(b_ref, 8, 0);
    lv_obj_add_event_cb(b_ref, on_fav_refresh_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * rl = lv_label_create(b_ref);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(rl);

    s_lbl_fav_msg = lv_label_create(card_right);
    lv_obj_set_style_text_font(s_lbl_fav_msg, SF(16), 0);
    lv_obj_set_style_text_color(s_lbl_fav_msg, lv_color_hex(THEME_ACCENT_BLUE), 0);
    lv_label_set_text(s_lbl_fav_msg, tr("Laden...", "Loading..."));
    lv_obj_align(s_lbl_fav_msg, LV_ALIGN_TOP_LEFT, SX(6), SY(52));

    s_fav_list = lv_obj_create(card_right);
    lv_obj_set_size(s_fav_list, SX(466), SY(418));
    lv_obj_set_pos(s_fav_list, 0, SY(74));
    lv_obj_set_style_bg_opa(s_fav_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fav_list, 0, 0);
    lv_obj_set_style_pad_all(s_fav_list, 0, 0);
    lv_obj_set_style_pad_row(s_fav_list, 8, 0);
    lv_obj_add_flag(s_fav_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_fav_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_fav_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_fav_list, LV_FLEX_FLOW_COLUMN);

    /* Start background items load */
    trigger_load_items();

    /* Timer for periodic state update */
    s_refresh_timer = lv_timer_create(sonos_modal_tick_cb, 500, NULL);
    sonos_modal_tick_cb(s_refresh_timer);

    return s_modal;
}

