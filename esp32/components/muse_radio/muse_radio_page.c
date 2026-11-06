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
 * The radio tile: a RADIO/SHOWS toggle, the station list under it, and a
 * player that replaces both while something is on.
 *
 * SHOWS is deliberately an empty state. The toggle was chosen (plan/radio.md)
 * so the podcast half could arrive later without moving anything, and this is
 * that later: radio works, SHOWS says so.
 *
 * Everything here runs on the LVGL task. The stream lives on its own two
 * tasks in muse_radio.c, and this file only reads its state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_page.h"
#include "muse_radio.h"

/* The palette muse_ui.c uses, so the page looks like the rest of the UI. */
#define COLOR_DIM     0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_ACCENT  0xa77dff
#define COLOR_LIT     0xf2efff
#define COLOR_PANEL   0x140f22
#define COLOR_WARN    0xffd060

/*
 * Sizes are set by the CJK font: its cell is 16 px, so a station name in
 * Chinese cannot be drawn smaller or larger than that, and the rows are built
 * around it. Four rows fit at this height; a fifth would run off the round
 * panel's edge.
 */
#define ROW_W 330
#define ROW_H 64
#define Y_TOGGLE (-166)
#define PILL_W 112
#define PILL_H 38
#define BTN_W 180
#define BTN_H 52

static lv_obj_t *s_list_view, *s_player_view;
static lv_obj_t *s_pill_radio, *s_pill_shows;
static lv_obj_t *s_rows, *s_empty;
static lv_obj_t *s_row[8], *s_row_name[8], *s_row_genre[8], *s_row_mark[8];
static lv_obj_t *s_now_name, *s_now_genre, *s_now_state, *s_now_title;
static bool s_shows;    /* which half of the toggle is up */
static volatile bool s_bench_open;   /* a console tune wants the player up */
static volatile int s_bench_half = -1;   /* a console toggle: 0 RADIO, 1 SHOWS */

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

/* ---------------------------------------------------------------- painting */

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
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_rows, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
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

static void show_player(bool on)
{
    if (on) {
        lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
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
    show_player(true);
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    muse_radio_stop();
    paint_rows();
    show_player(false);
}

/* Leaves it playing and goes back to the list. */
static void on_back(lv_event_t *e)
{
    (void)e;
    paint_rows();
    show_player(false);
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

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color,
                        int16_t dy, lv_event_cb_t cb)
{
    lv_obj_t *b = panel(parent, BTN_W, BTN_H, BTN_H / 2);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, dy);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    /*
     * The tile is a tileview, which scrolls. A press that drifts more than the
     * indev's scroll limit hands the gesture to the tileview, the button gets
     * PRESS_LOST and no CLICKED ever arrives -- which is why STOP and BACK
     * worked only sometimes. Breaking the scroll chain at the button keeps the
     * press. The cost is that a swipe *started on a button* no longer turns
     * the page; starting it anywhere else still does.
     */
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, &lv_font_unscii_16, color, 0, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_text(l, text);
    return b;
}

static void build_list(lv_obj_t *parent)
{
    s_pill_radio = pill(parent, "RADIO", -(PILL_W / 2 + 4), false);
    s_pill_shows = pill(parent, "SHOWS", PILL_W / 2 + 4, true);

    /* A flex column so the rows stay centred however many stations the table
     * holds, instead of each one carrying a hand-placed offset. */
    s_rows = lv_obj_create(parent);
    lv_obj_remove_style_all(s_rows);
    lv_obj_remove_flag(s_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_rows, ROW_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_rows, 8, 0);
    lv_obj_align(s_rows, LV_ALIGN_CENTER, 0, 10);

    for (int i = 0; i < muse_radio_count() && i < 8; i++) {
        const muse_radio_station_t *st = muse_radio_station(i);
        s_row[i] = panel(s_rows, ROW_W, ROW_H, 12);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_row[i], on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        s_row_name[i] = label(s_row[i], muse_page_caption_font(), COLOR_LIT, 0, -13);
        lv_obj_align(s_row_name[i], LV_ALIGN_LEFT_MID, 16, -13);
        lv_label_set_text(s_row_name[i], st->name);

        s_row_genre[i] = label(s_row[i], muse_page_caption_font(), COLOR_DIM, 0, 13);
        lv_obj_align(s_row_genre[i], LV_ALIGN_LEFT_MID, 16, 13);
        lv_label_set_text(s_row_genre[i], st->genre);

        s_row_mark[i] = label(s_row[i], &lv_font_unscii_16, COLOR_ACCENT, 0, 0);
        lv_obj_align(s_row_mark[i], LV_ALIGN_RIGHT_MID, -18, 0);
    }

    s_empty = lv_obj_create(parent);
    lv_obj_remove_style_all(s_empty);
    lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_empty, ROW_W, 120);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 10);
    lv_obj_t *l = label(s_empty, &lv_font_unscii_16, COLOR_DIM, 0, -14);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_text(l, "NO SHOWS YET");
    lv_obj_t *c = label(s_empty, &lv_font_unscii_16, COLOR_DIM, 0, 16);
    lv_label_set_text(c, "PODCASTS COME LATER");
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
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

static void build(lv_obj_t *tile)
{
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    s_list_view = lv_obj_create(tile);
    lv_obj_remove_style_all(s_list_view);
    lv_obj_remove_flag(s_list_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_list_view, lv_pct(100), lv_pct(100));
    build_list(s_list_view);

    s_player_view = lv_obj_create(tile);
    lv_obj_remove_style_all(s_player_view);
    lv_obj_remove_flag(s_player_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_player_view, lv_pct(100), lv_pct(100));
    build_player(s_player_view);

    /* The toggle starts on RADIO every time the screen is built, which is the
     * decision in plan/radio.md: the page is the same page on every swipe. */
    s_shows = false;
    paint_toggle();
    paint_rows();
    show_player(false);
}

static void tick(bool visible)
{
    /* Not gated on `visible`: this is what gives the speaker back to Muse, and
     * it has to happen whichever page you are looking at. */
    muse_radio_tick();
    if (s_bench_half >= 0) {
        s_shows = s_bench_half == 1;
        s_bench_half = -1;
        paint_toggle();
        show_player(false);
    }
    if (s_bench_open && muse_radio_index() >= 0) {
        s_bench_open = false;
        paint_player();
        show_player(true);
    }
    if (!visible) {
        return;
    }
    paint_rows();
    if (!lv_obj_has_flag(s_player_view, LV_OBJ_FLAG_HIDDEN)) {
        if (muse_radio_index() < 0) {
            show_player(false);   /* Muse took the speaker, or it gave up */
        } else {
            paint_player();
        }
    }
}

/*
 * `page=radio:play=1`, `:stop`, `:state`, `:list` and `:shows` on the console.
 * There is no way to tap the glass over the USB cable, so this is how the
 * stream gets exercised on the board at all. Console task: no LVGL here, only
 * the flag that tick() picks up.
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
        return true;
    }
    if (!strcmp(arg, "shows") || !strcmp(arg, "list")) {
        s_bench_half = !strcmp(arg, "shows");
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
    return false;
}

static const muse_page_t radio_page = {
    .name = "radio",
    .build = build,
    .tick = tick,
    .bench = bench,
};
MUSE_PAGE(radio_page);
