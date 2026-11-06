/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The radio tile: a RADIO/SHOWS toggle and, under it, one of four screens.
 *
 *   RADIO -> the station list -> the live player
 *   SHOWS -> the show list -> that show's episodes -> the episode player
 *
 * The toggle was chosen (plan/radio.md) so the podcast half could arrive
 * without moving anything, and the SHOWS side is deeper by one screen because
 * option B was taken: a show's episodes get a list of their own rather than
 * the newest one playing on a tap.
 *
 * Everything here runs on the LVGL task. Both engines live on tasks of their
 * own in muse_radio.c and muse_podcast.c, and this file only reads their
 * state -- with the exception of the three calls that start work and return
 * at once (radio play, feed load, episode play).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_page.h"
#include "muse_podcast.h"
#include "muse_radio.h"

/* The palette muse_ui.c uses, so the page looks like the rest of the UI. */
#define COLOR_DIM     0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_ACCENT  0xa77dff
#define COLOR_LIT     0xf2efff
#define COLOR_PANEL   0x140f22
#define COLOR_WARN    0xffd060

/*
 * Sizes are set by the font: without CONFIG_MUSE_PAGE_CJK_24 a name is in the
 * CJK fallback's 16 px cell and cannot be drawn larger, and with it a page's
 * own names are 24 px while anything off the network stays at 16. The lists
 * scroll, so the number of rows is no longer bounded by the glass -- which is
 * what lets seven shows and six stations both fit.
 */
#define ROW_W 330
#if CONFIG_MUSE_PAGE_CJK_24
/* A 24 px name over a 16 px genre, so the row grows and six of them no longer
 * fit the circle -- which is what SCROLL_H below is for. */
#define ROW_H 56
#define NAME_DY (-12)
#define GENRE_DY 14
#define SHOW_H 46      /* one line, so a show row is shorter than a station's */
#else
#define ROW_H 48
#define NAME_DY (-10)
#define GENRE_DY 10
#define SHOW_H 40
#endif
#define EP_H 58
/* The window the rows scroll inside: its top clears the toggle, its bottom
 * clears the page dots, and the circle is still wider than a row at both
 * edges. The tileview takes LV_DIR_LEFT | LV_DIR_RIGHT only, so a vertical
 * drag has no other claimant and turning pages is unaffected. */
#define SCROLL_H 300
/* Exactly the four rows the parser keeps, so the list never shows half of a
 * fifth and the window needs no scrolling of its own. */
#define EP_SCROLL_H (4 * EP_H + 3 * 5)
#define Y_TOGGLE (-166)
#define PILL_W 112
#define PILL_H 38
#define BTN_W 180
#define BTN_H 52
#define BAR_W 300
/* How long a marqueed title takes to cross its row. LVGL's default gallops;
 * this is about a character every 150 ms, which is readable rather than
 * decorative, and four rows of it at once is still calm. */
#define EP_SCROLL_MS 9000

/* Which of the four screens is up. */
typedef enum {
    V_LIST = 0,     /* the toggle, over either the stations or the shows */
    V_RADIO,        /* the live player */
    V_EPISODES,     /* one show's newest episodes */
    V_PODCAST,      /* the episode player */
} view_t;

static lv_obj_t *s_view[4];
static view_t s_at = V_LIST;

static lv_obj_t *s_pill_radio, *s_pill_shows;
static lv_obj_t *s_rows, *s_shows_box;
static lv_obj_t *s_row[8], *s_row_name[8], *s_row_genre[8], *s_row_mark[8];
static lv_obj_t *s_show_row[8], *s_show_name[8], *s_show_mark[8];
static lv_obj_t *s_now_name, *s_now_genre, *s_now_state, *s_now_title;

/* The episode list. */
static lv_obj_t *s_ep_head, *s_ep_box, *s_ep_note;
static lv_obj_t *s_ep_row[MUSE_RSS_ITEMS_MAX];
static lv_obj_t *s_ep_title[MUSE_RSS_ITEMS_MAX], *s_ep_sub[MUSE_RSS_ITEMS_MAX];

/* The episode player. */
static lv_obj_t *s_pod_show, *s_pod_title, *s_pod_state, *s_pod_bar;
static lv_obj_t *s_pod_at, *s_pod_left, *s_pod_pause_label;

static bool s_shows;    /* which half of the toggle is up */
static int s_open_show = -1;   /* whose episodes the list is showing */

static volatile bool s_bench_open;        /* a console tune wants the player up */
static volatile int s_bench_half = -1;    /* a console toggle: 0 RADIO, 1 SHOWS */
static volatile int s_bench_scroll = -1;  /* a console scroll, in pixels */
static volatile int s_bench_eps = -1;     /* open this show's episode list */
static volatile int s_bench_view = -1;    /* jump straight to a view */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       int16_t dx, int16_t dy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
    return l;
}

static lv_obj_t *panel(lv_obj_t *parent, int16_t w, int16_t h, int16_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* A column of rows that scrolls vertically and nothing else. The tileview owns
 * the horizontal axis and turns pages; SCROLL_CHAIN off so an over-scroll at
 * either end stops here instead of becoming a page turn. */
static lv_obj_t *scroller(lv_obj_t *parent, int16_t h, int16_t dy)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, ROW_W, h);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, 5, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, dy);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(o, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 2, LV_PART_SCROLLBAR);
    return o;
}

static void mmss(int secs, char *out, size_t cap)
{
    if (secs < 0) {
        secs = 0;
    }
    if (secs >= 3600) {
        snprintf(out, cap, "%d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
    } else {
        snprintf(out, cap, "%d:%02d", secs / 60, secs % 60);
    }
}

/* ---------------------------------------------------------------- painting */

static void show_view(view_t v)
{
    s_at = v;
    for (int i = 0; i < 4; i++) {
        if (i == (int)v) {
            lv_obj_remove_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void paint_toggle(void)
{
    lv_obj_set_style_bg_color(s_pill_radio, lv_color_hex(s_shows ? COLOR_PANEL : COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(s_pill_shows, lv_color_hex(s_shows ? COLOR_ACCENT : COLOR_PANEL), 0);
    lv_obj_t *lr = lv_obj_get_child(s_pill_radio, 0);
    lv_obj_t *ls = lv_obj_get_child(s_pill_shows, 0);
    /* The lit pill carries the dark text, so the accent stays readable. */
    lv_obj_set_style_text_color(lr, lv_color_hex(s_shows ? COLOR_DIM : COLOR_PANEL), 0);
    lv_obj_set_style_text_color(ls, lv_color_hex(s_shows ? COLOR_PANEL : COLOR_DIM), 0);

    if (s_shows) {
        lv_obj_add_flag(s_rows, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_shows_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_rows, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shows_box, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The tuned station's row carries a dot, so the list says where the player
 * would take you back to. */
static void paint_rows(void)
{
    int tuned = muse_radio_index();
    for (int i = 0; i < muse_radio_count() && i < 8; i++) {
        bool on = i == tuned;
        lv_obj_set_style_text_color(s_row_name[i], lv_color_hex(on ? COLOR_ACCENT : COLOR_LIT), 0);
        lv_label_set_text(s_row_mark[i], on ? "*" : "");
    }
}

/* Same idea for the shows: the one with an episode on carries the dot. */
static void paint_shows(void)
{
    int playing = muse_podcast_playing_show();
    for (int i = 0; i < muse_podcast_show_count() && i < 8; i++) {
        bool on = i == playing;
        lv_obj_set_style_text_color(s_show_name[i], lv_color_hex(on ? COLOR_ACCENT : COLOR_LIT), 0);
        lv_label_set_text(s_show_mark[i], on ? "*" : ">");
        lv_obj_set_style_text_color(s_show_mark[i], lv_color_hex(on ? COLOR_ACCENT : COLOR_DIM), 0);
    }
}

static void paint_player(void)
{
    const muse_radio_station_t *st = muse_radio_station(muse_radio_index());
    char title[96];
    uint32_t color = COLOR_CAPTION;
    const char *state;

    if (!st) {
        return;
    }
    lv_label_set_text(s_now_name, st->name);
    lv_label_set_text(s_now_genre, st->genre);

    switch (muse_radio_state()) {
    case MUSE_RADIO_CONNECTING: state = "CONNECTING"; break;
    case MUSE_RADIO_BUFFERING:  state = "BUFFERING"; color = COLOR_WARN; break;
    case MUSE_RADIO_PLAYING:    state = "ON AIR"; color = COLOR_ACCENT; break;
    case MUSE_RADIO_FAILED:
        state = muse_radio_status_text();
        color = COLOR_WARN;
        break;
    default: state = "STOPPED"; break;
    }
    lv_label_set_text(s_now_state, state);
    lv_obj_set_style_text_color(s_now_state, lv_color_hex(color), 0);

    /* Not every station sends ICY metadata -- France Inter sends none -- so
     * the line is empty rather than guessed at. */
    muse_radio_title(title, sizeof(title));
    lv_label_set_text(s_now_title, title);
}

/* The episode list: a note while the feed is in flight or has failed, the rows
 * once it is in. */
static void paint_episodes(void)
{
    const muse_podcast_show_t *sh = muse_podcast_show(s_open_show);
    lv_label_set_text(s_ep_head, sh ? sh->name : "");

    muse_feed_state_t fs = muse_podcast_feed_state();
    int n = muse_podcast_feed_show() == s_open_show ? muse_podcast_episode_count() : 0;

    if (fs == MUSE_FEED_FETCHING) {
        lv_label_set_text(s_ep_note, "LOADING...");
        lv_obj_set_style_text_color(s_ep_note, lv_color_hex(COLOR_DIM), 0);
    } else if (fs == MUSE_FEED_FAILED) {
        lv_label_set_text(s_ep_note, muse_podcast_feed_status());
        lv_obj_set_style_text_color(s_ep_note, lv_color_hex(COLOR_WARN), 0);
    } else {
        lv_label_set_text(s_ep_note, "");
    }
    if (n > 0) {
        lv_obj_add_flag(s_ep_note, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ep_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ep_note, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ep_box, LV_OBJ_FLAG_HIDDEN);
    }

    int playing = muse_podcast_playing_show() == s_open_show ? muse_podcast_playing_episode() : -1;
    for (int i = 0; i < MUSE_RSS_ITEMS_MAX; i++) {
        const muse_rss_item_t *ep = i < n ? muse_podcast_episode(i) : NULL;
        if (!ep) {
            lv_obj_add_flag(s_ep_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ep_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ep_title[i], ep->title);
        lv_obj_set_style_text_color(s_ep_title[i],
                                    lv_color_hex(i == playing ? COLOR_ACCENT : COLOR_LIT), 0);

        /* "54 分   11/04   38%", the last part only when there is a resume
         * point for this very episode. The fields are held apart by spaces
         * rather than a middle dot: neither unscii nor the CJK fallback has
         * U+00B7, and a missing glyph draws as an empty box. */
        char sub[64];
        int at = 0;
        if (ep->secs > 0) {
            at += snprintf(sub + at, sizeof(sub) - at, "%d 分", (ep->secs + 30) / 60);
        }
        if (ep->month) {
            at += snprintf(sub + at, sizeof(sub) - at, "%s%02d/%02d", at ? "   " : "",
                           ep->month, ep->day);
        }
        int mark = muse_podcast_resume_mark(i);
        if (mark >= 0) {
            snprintf(sub + at, sizeof(sub) - at, "%s%d%%", at ? "   " : "", (mark + 5) / 10);
        }
        lv_label_set_text(s_ep_sub[i], sub);
        lv_obj_set_style_text_color(s_ep_sub[i],
                                    lv_color_hex(mark >= 0 ? COLOR_ACCENT : COLOR_DIM), 0);
    }
}

static void paint_podcast(void)
{
    const muse_podcast_show_t *sh = muse_podcast_show(muse_podcast_playing_show());
    char buf[24];
    uint32_t color = COLOR_ACCENT;
    const char *state;

    lv_label_set_text(s_pod_show, sh ? sh->name : "");
    lv_label_set_text(s_pod_title, muse_podcast_playing_title());

    switch (muse_podcast_state()) {
    case MUSE_EP_CONNECTING: state = "CONNECTING"; color = COLOR_CAPTION; break;
    case MUSE_EP_BUFFERING:  state = "BUFFERING"; color = COLOR_WARN; break;
    case MUSE_EP_PLAYING:    state = "PLAYING"; break;
    case MUSE_EP_PAUSED:     state = "PAUSED"; color = COLOR_CAPTION; break;
    case MUSE_EP_FINISHED:   state = "FINISHED"; color = COLOR_CAPTION; break;
    case MUSE_EP_FAILED:     state = muse_podcast_status_text(); color = COLOR_WARN; break;
    default:                 state = "STOPPED"; color = COLOR_CAPTION; break;
    }
    lv_label_set_text(s_pod_state, state);
    lv_obj_set_style_text_color(s_pod_state, lv_color_hex(color), 0);
    lv_label_set_text(s_pod_pause_label, muse_podcast_is_paused() ? "PLAY" : "PAUSE");

    int pos = muse_podcast_position_secs();
    int dur = muse_podcast_duration_secs();
    /* The bar is the byte fraction, which is exact; the clock either side of
     * it goes through the average bitrate, so on a variable-bitrate episode
     * the numbers drift from the bar by a few seconds. */
    lv_bar_set_value(s_pod_bar, dur > 0 ? (pos * 1000) / dur : 0, LV_ANIM_OFF);
    mmss(pos, buf, sizeof(buf));
    lv_label_set_text(s_pod_at, buf);
    if (dur > 0) {
        mmss(dur - pos, buf, sizeof(buf));
        lv_label_set_text_fmt(s_pod_left, "-%s", buf);
    } else {
        lv_label_set_text(s_pod_left, "");
    }
}

/* ----------------------------------------------------------------- the taps */

static void on_pill(lv_event_t *e)
{
    s_shows = (bool)(intptr_t)lv_event_get_user_data(e);
    paint_toggle();
}

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    muse_radio_play(i);
    paint_rows();
    paint_player();
    show_view(V_RADIO);
}

static void on_show(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    /* Nothing is fetched until a show is tapped, which is the cost of option B
     * being cheap on a swipe: the first episode list of a session waits on the
     * network, and the screen says LOADING while it does. */
    s_open_show = i;
    muse_podcast_load(i);
    paint_episodes();
    show_view(V_EPISODES);
}

static void on_episode(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!muse_podcast_play(i)) {
        return;
    }
    paint_podcast();
    show_view(V_PODCAST);
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    muse_radio_stop();
    paint_rows();
    show_view(V_LIST);
}

/* Leaves it playing and goes back to the list. */
static void on_back(lv_event_t *e)
{
    (void)e;
    paint_rows();
    show_view(V_LIST);
}

static void on_ep_back(lv_event_t *e)
{
    (void)e;
    s_shows = true;
    paint_toggle();
    paint_shows();
    show_view(V_LIST);
}

/* B's one real cost: BACK from the player goes to the episode list, not to
 * the shows, so the page carries a two-deep stack where radio had none. */
static void on_pod_back(lv_event_t *e)
{
    (void)e;
    if (s_open_show < 0) {
        on_ep_back(e);
        return;
    }
    paint_episodes();
    show_view(V_EPISODES);
}

static void on_pause(lv_event_t *e)
{
    (void)e;
    muse_podcast_pause(!muse_podcast_is_paused());
    paint_podcast();
}

static void on_seek(lv_event_t *e)
{
    muse_podcast_seek((int)(intptr_t)lv_event_get_user_data(e));
    paint_podcast();
}

/* ------------------------------------------------------------------- build */

static lv_obj_t *pill(lv_obj_t *parent, const char *text, int16_t dx, bool shows)
{
    lv_obj_t *p = panel(parent, PILL_W, PILL_H, PILL_H / 2);
    lv_obj_align(p, LV_ALIGN_CENTER, dx, Y_TOGGLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLL_CHAIN);   /* as the buttons, below */
    lv_obj_add_event_cb(p, on_pill, LV_EVENT_CLICKED, (void *)(intptr_t)shows);
    lv_obj_t *l = label(p, &lv_font_unscii_16, COLOR_DIM, 0, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_text(l, text);
    return p;
}

static lv_obj_t *button_at(lv_obj_t *parent, const char *text, uint32_t color, int16_t w,
                           int16_t dx, int16_t dy, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = panel(parent, w, BTN_H, BTN_H / 2);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    /*
     * The tile is a tileview, which scrolls. A press that drifts more than the
     * indev's scroll limit hands the gesture to the tileview, the button gets
     * PRESS_LOST and no CLICKED ever arrives -- which is why STOP and BACK
     * worked only sometimes. Breaking the scroll chain at the button keeps the
     * press. The cost is that a swipe *started on a button* no longer turns
     * the page; starting it anywhere else still does. Every button added for
     * the podcast half needs this too.
     */
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *l = label(b, &lv_font_unscii_16, color, 0, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_text(l, text);
    return b;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color,
                        int16_t dy, lv_event_cb_t cb)
{
    return button_at(parent, text, color, BTN_W, 0, dy, cb, NULL);
}

/* A row in one of the lists: a panel that takes a tap and keeps it. */
static lv_obj_t *row(lv_obj_t *box, int16_t h, lv_event_cb_t cb, int i)
{
    lv_obj_t *r = panel(box, ROW_W, h, 12);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return r;
}

static void build_list(lv_obj_t *parent)
{
    s_pill_radio = pill(parent, "RADIO", -(PILL_W / 2 + 4), false);
    s_pill_shows = pill(parent, "SHOWS", PILL_W / 2 + 4, true);

    /* A flex column so the rows stay centred however many stations the table
     * holds, instead of each one carrying a hand-placed offset. */
    s_rows = scroller(parent, SCROLL_H, 26);
    for (int i = 0; i < muse_radio_count() && i < 8; i++) {
        const muse_radio_station_t *st = muse_radio_station(i);
        s_row[i] = row(s_rows, ROW_H, on_row, i);

        s_row_name[i] = label(s_row[i], muse_page_list_font(), COLOR_LIT, 0, -10);
        lv_obj_align(s_row_name[i], LV_ALIGN_LEFT_MID, 16, NAME_DY);
        lv_label_set_text(s_row_name[i], st->name);

        s_row_genre[i] = label(s_row[i], muse_page_list_sub_font(), COLOR_DIM, 0, 10);
        lv_obj_align(s_row_genre[i], LV_ALIGN_LEFT_MID, 16, GENRE_DY);
        lv_label_set_text(s_row_genre[i], st->genre);

        s_row_mark[i] = label(s_row[i], &lv_font_unscii_16, COLOR_ACCENT, 0, 0);
        lv_obj_align(s_row_mark[i], LV_ALIGN_RIGHT_MID, -18, 0);
    }

    /* The shows. One line each, because under option B the episode lives on
     * the next screen rather than in a caption -- which is how seven of them
     * fit where six stations was the limit. */
    s_shows_box = scroller(parent, SCROLL_H, 26);
    for (int i = 0; i < muse_podcast_show_count() && i < 8; i++) {
        const muse_podcast_show_t *sh = muse_podcast_show(i);
        s_show_row[i] = row(s_shows_box, SHOW_H, on_show, i);

        s_show_name[i] = label(s_show_row[i], muse_page_list_font(), COLOR_LIT, 0, 0);
        lv_obj_align(s_show_name[i], LV_ALIGN_LEFT_MID, 16, 0);
        lv_label_set_text(s_show_name[i], sh->name);

        s_show_mark[i] = label(s_show_row[i], &lv_font_unscii_16, COLOR_DIM, 0, 0);
        lv_obj_align(s_show_mark[i], LV_ALIGN_RIGHT_MID, -18, 0);
    }
    lv_obj_add_flag(s_shows_box, LV_OBJ_FLAG_HIDDEN);
}

static void build_player(lv_obj_t *parent)
{
    s_now_name = label(parent, muse_page_caption_font(), COLOR_LIT, 0, -110);
    s_now_genre = label(parent, muse_page_caption_font(), COLOR_DIM, 0, -80);

    s_now_state = label(parent, &lv_font_unscii_16, COLOR_ACCENT, 0, -34);
    lv_obj_set_style_text_letter_space(s_now_state, 3, 0);

    /* An ICY title is as long as the station feels like making it, so it
     * scrolls rather than widening the page. */
    s_now_title = label(parent, muse_page_caption_font(), COLOR_CAPTION, 0, 6);
    lv_obj_set_width(s_now_title, 320);
    lv_label_set_long_mode(s_now_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_now_title, LV_TEXT_ALIGN_CENTER, 0);

    button(parent, "STOP", COLOR_ACCENT, 70, on_stop);
    button(parent, "BACK", COLOR_DIM, 132, on_back);
}

static void build_episodes(lv_obj_t *parent)
{
    s_ep_head = label(parent, muse_page_list_font(), COLOR_ACCENT, 0, -170);
    lv_obj_set_width(s_ep_head, ROW_W);
    lv_label_set_long_mode(s_ep_head, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_ep_head, LV_TEXT_ALIGN_CENTER, 0);

    s_ep_note = label(parent, &lv_font_unscii_16, COLOR_DIM, 0, -10);
    lv_obj_set_style_text_letter_space(s_ep_note, 2, 0);

    /* Four is what the circle allows once the header and BACK have taken a row
     * each (plan/radio.md), and it is also all the parser keeps. */
    s_ep_box = scroller(parent, EP_SCROLL_H, -8);
    for (int i = 0; i < MUSE_RSS_ITEMS_MAX; i++) {
        s_ep_row[i] = row(s_ep_box, EP_H, on_episode, i);

        /*
         * An episode title came off the network, so the 24 px subset cannot
         * draw it -- that one holds only the characters the firmware owns.
         * muse_page_text_font() is the next best thing: one anti-aliased face
         * for the Latin and the Chinese both, instead of unscii's pixels
         * beside a fallback.
         *
         * And it marquees rather than ending in dots. These titles carry the
         * episode number in front, so the part that gets cut is the part
         * worth reading; LVGL only animates a label that actually overflows,
         * so short titles sit still.
         */
        s_ep_title[i] = label(s_ep_row[i], muse_page_text_font(), COLOR_LIT, 0, -11);
        lv_obj_set_width(s_ep_title[i], ROW_W - 36);
        lv_label_set_long_mode(s_ep_title[i], LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(s_ep_title[i], EP_SCROLL_MS, 0);
        lv_obj_align(s_ep_title[i], LV_ALIGN_LEFT_MID, 16, -11);

        s_ep_sub[i] = label(s_ep_row[i], muse_page_text_font(), COLOR_DIM, 0, 13);
        lv_obj_align(s_ep_sub[i], LV_ALIGN_LEFT_MID, 16, 13);

        lv_obj_add_flag(s_ep_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    button(parent, "SHOWS", COLOR_DIM, 150, on_ep_back);
}

static void build_podcast(lv_obj_t *parent)
{
    s_pod_show = label(parent, muse_page_list_font(), COLOR_DIM, 0, -150);

    s_pod_title = label(parent, muse_page_text_font(), COLOR_LIT, 0, -108);
    lv_obj_set_width(s_pod_title, 330);
    lv_label_set_long_mode(s_pod_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_pod_title, EP_SCROLL_MS, 0);
    lv_obj_set_style_text_align(s_pod_title, LV_TEXT_ALIGN_CENTER, 0);

    s_pod_state = label(parent, &lv_font_unscii_16, COLOR_ACCENT, 0, -62);
    lv_obj_set_style_text_letter_space(s_pod_state, 3, 0);

    /* An episode has a length, so unlike the radio's player there is somewhere
     * to be in it. */
    s_pod_bar = lv_bar_create(parent);
    lv_obj_remove_style_all(s_pod_bar);
    lv_obj_set_size(s_pod_bar, BAR_W, 8);
    lv_obj_align(s_pod_bar, LV_ALIGN_CENTER, 0, -22);
    lv_obj_set_style_radius(s_pod_bar, 4, 0);
    lv_obj_set_style_bg_color(s_pod_bar, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(s_pod_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pod_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_pod_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_pod_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_pod_bar, 0, 1000);
    lv_bar_set_value(s_pod_bar, 0, LV_ANIM_OFF);

    /* Each clock gets half the bar's width and is aligned to its own end, so
     * neither drifts past the bar as the digits change. */
    s_pod_at = label(parent, &lv_font_unscii_16, COLOR_CAPTION, 0, 2);
    lv_obj_set_width(s_pod_at, BAR_W / 2);
    lv_obj_set_style_text_align(s_pod_at, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_pod_at, LV_ALIGN_CENTER, -(BAR_W / 4), 2);
    s_pod_left = label(parent, &lv_font_unscii_16, COLOR_DIM, 0, 2);
    lv_obj_set_width(s_pod_left, BAR_W / 2);
    lv_obj_set_style_text_align(s_pod_left, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_pod_left, LV_ALIGN_CENTER, BAR_W / 4, 2);

    button_at(parent, "-30", COLOR_DIM, 84, -112, 70, on_seek, (void *)(intptr_t)-30);
    lv_obj_t *p = button_at(parent, "PAUSE", COLOR_ACCENT, 120, 0, 70, on_pause, NULL);
    s_pod_pause_label = lv_obj_get_child(p, 0);
    button_at(parent, "+30", COLOR_DIM, 84, 112, 70, on_seek, (void *)(intptr_t)30);

    button(parent, "EPISODES", COLOR_DIM, 140, on_pod_back);
}

static void build(lv_obj_t *tile)
{
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        s_view[i] = lv_obj_create(tile);
        lv_obj_remove_style_all(s_view[i]);
        lv_obj_remove_flag(s_view[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_view[i], lv_pct(100), lv_pct(100));
    }
    build_list(s_view[V_LIST]);
    build_player(s_view[V_RADIO]);
    build_episodes(s_view[V_EPISODES]);
    build_podcast(s_view[V_PODCAST]);

    /* The toggle starts on RADIO every time the screen is built, which is the
     * decision in plan/radio.md: the page is the same page on every swipe. */
    s_shows = false;
    paint_toggle();
    paint_rows();
    paint_shows();
    show_view(V_LIST);
}

static void tick(bool visible)
{
    /* Not gated on `visible`: this is what gives the speaker back to Muse, and
     * it has to happen whichever page you are looking at. */
    muse_radio_tick();
    muse_podcast_tick();

    if (s_bench_half >= 0) {
        s_shows = s_bench_half == 1;
        s_bench_half = -1;
        paint_toggle();
        show_view(V_LIST);
    }
    if (s_bench_scroll >= 0) {
        lv_obj_t *box = s_at == V_EPISODES ? s_ep_box : (s_shows ? s_shows_box : s_rows);
        if (box) {
            lv_obj_scroll_to_y(box, s_bench_scroll, LV_ANIM_OFF);
        }
        s_bench_scroll = -1;
    }
    if (s_bench_eps >= 0) {
        s_open_show = s_bench_eps;
        s_bench_eps = -1;
        paint_episodes();
        show_view(V_EPISODES);
    }
    if (s_bench_view >= 0) {
        view_t v = (view_t)s_bench_view;
        s_bench_view = -1;
        if (v == V_PODCAST) {
            paint_podcast();
        }
        show_view(v);
    }
    if (s_bench_open && muse_radio_index() >= 0) {
        s_bench_open = false;
        paint_player();
        show_view(V_RADIO);
    }
    if (!visible) {
        return;
    }
    switch (s_at) {
    case V_LIST:
        paint_rows();
        paint_shows();
        break;
    case V_RADIO:
        if (muse_radio_index() < 0) {
            show_view(V_LIST);   /* Muse took the speaker, or it gave up */
        } else {
            paint_player();
        }
        break;
    case V_EPISODES:
        paint_episodes();
        break;
    case V_PODCAST:
        /* A finished or failed episode stays on screen and says so, rather
         * than dropping the list back under the user's thumb. */
        if (muse_podcast_playing_show() < 0) {
            show_view(V_EPISODES);
        } else {
            paint_podcast();
        }
        break;
    }
}

/*
 * `page=radio:` plus `play=N`, `stop`, `state`, `list`, `shows`, `scroll=N`,
 * `show=N`, `ep=N`, `pause`, `resume`, `seek=N`, `pstate`, `view=N`.
 * There is no way to tap the glass over the USB cable, so this is how both
 * halves get exercised on the board at all. Console task: no LVGL here, only
 * the flags that tick() picks up.
 */
static bool bench(const char *arg)
{
    if (!strncmp(arg, "play=", 5)) {
        if (!muse_radio_play(atoi(arg + 5))) {
            return false;
        }
        s_bench_open = true;
        return true;
    }
    if (!strcmp(arg, "stop")) {
        muse_radio_stop();
        muse_podcast_stop();
        return true;
    }
    if (!strncmp(arg, "scroll=", 7)) {
        s_bench_scroll = atoi(arg + 7);
        return true;
    }
    if (!strcmp(arg, "shows") || !strcmp(arg, "list")) {
        s_bench_half = !strcmp(arg, "shows");
        return true;
    }
    if (!strncmp(arg, "show=", 5)) {
        int i = atoi(arg + 5);
        if (!muse_podcast_load(i)) {
            return false;
        }
        s_bench_eps = i;
        return true;
    }
    if (!strncmp(arg, "ep=", 3)) {
        if (!muse_podcast_play(atoi(arg + 3))) {
            return false;
        }
        s_bench_view = V_PODCAST;
        return true;
    }
    if (!strncmp(arg, "view=", 5)) {
        int v = atoi(arg + 5);
        if (v < 0 || v > 3) {
            return false;
        }
        s_bench_view = v;
        return true;
    }
    if (!strcmp(arg, "pause") || !strcmp(arg, "resume")) {
        muse_podcast_pause(!strcmp(arg, "pause"));
        return true;
    }
    if (!strncmp(arg, "seek=", 5)) {
        muse_podcast_seek(atoi(arg + 5));
        return true;
    }
    if (!strcmp(arg, "state")) {
        char title[96];
        muse_radio_title(title, sizeof(title));
        printf("@radio state=%d station=%d status=\"%s\" title=\"%s\"\n",
               (int)muse_radio_state(), muse_radio_index(), muse_radio_status_text(), title);
        fflush(stdout);
        return true;
    }
    if (!strcmp(arg, "pstate")) {
        printf("@podcast feed=%d/%d episodes=%d state=%d show=%d ep=%d "
               "pos=%d/%d paused=%d status=\"%s\"\n",
               (int)muse_podcast_feed_state(), muse_podcast_feed_show(),
               muse_podcast_episode_count(), (int)muse_podcast_state(),
               muse_podcast_playing_show(), muse_podcast_playing_episode(),
               muse_podcast_position_secs(), muse_podcast_duration_secs(),
               (int)muse_podcast_is_paused(), muse_podcast_status_text());
        for (int i = 0; i < muse_podcast_episode_count(); i++) {
            const muse_rss_item_t *ep = muse_podcast_episode(i);
            printf("@episode %d secs=%d date=%02d/%02d resume=%d title=\"%s\"\n", i, ep->secs,
                   ep->month, ep->day, muse_podcast_resume_mark(i), ep->title);
        }
        fflush(stdout);
        return true;
    }
    return false;
}

static const muse_page_t radio_page = {
    .name = "radio",
    .build = build,
    .tick = tick,
    .bench = bench,
};
MUSE_PAGE(radio_page);
