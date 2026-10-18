#include "mp3_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "bsp_esp323248s035.h"
#include "mp3dec.h"

static const char *TAG = "mp3";

#define EV_START     BIT0
#define IN_BUF       4096
#define MAX_FRAMES   1152        /* MPEG-1 layer III samples per frame */

enum { ST_IDLE, ST_PLAYING, ST_PAUSED, ST_DONE, ST_ERROR };

static volatile int s_state = ST_IDLE;
static volatile bool s_stop_req;
static volatile uint32_t s_pos_frames;
static volatile uint32_t s_total_frames;
static volatile uint32_t s_sample_rate;

static char s_path[512];
static char s_title[128];

static EventGroupHandle_t s_evt;
static TaskHandle_t s_task;

/* Decoder scratch buffers live in .bss so the task stack can stay small. */
static uint8_t s_inbuf[IN_BUF];
static int16_t s_pcm[2 * MAX_FRAMES];
static int16_t s_mono[MAX_FRAMES];

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_overlay;
static lv_obj_t *s_slider;
static lv_obj_t *s_elapsed;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_vol;
static lv_obj_t *s_vol_lbl;
static lv_timer_t *s_timer;

static int s_vol_pct = 20;      /* player volume, percent */

static const char *state_text(void)
{
    switch (s_state) {
    case ST_PLAYING: return "Playing";
    case ST_PAUSED:  return "Paused";
    case ST_DONE:    return "Finished";
    case ST_ERROR:   return "No audio";
    default:         return "Stopped";
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t total = s_total_frames;
    uint32_t pos = s_pos_frames;

    if (total) {
        uint32_t v = (uint32_t)(((uint64_t)pos * 1000) / total);
        if (v > 1000) v = 1000;
        lv_slider_set_value(s_slider, v, LV_ANIM_OFF);
    }
    if (s_state == ST_DONE) lv_slider_set_value(s_slider, 1000, LV_ANIM_OFF);

    uint32_t sr = s_sample_rate ? s_sample_rate : 44100;
    uint32_t el = pos / sr;
    uint32_t tot = total / sr;
    lv_label_set_text_fmt(s_elapsed, "%02" PRIu32 ":%02" PRIu32 " / %02" PRIu32 ":%02" PRIu32,
                          el / 60, el % 60, tot / 60, tot % 60);
    lv_label_set_text(s_state_lbl, state_text());
}

static void cb_play(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_PAUSED) {
        s_state = ST_PLAYING;
    } else if (s_state == ST_IDLE || s_state == ST_DONE || s_state == ST_ERROR) {
        xEventGroupSetBits(s_evt, EV_START);
    }
}

static void cb_pause(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_PLAYING) s_state = ST_PAUSED;
}

static void cb_stop(lv_event_t *e)
{
    (void)e;
    s_stop_req = true;
}

static void cb_volume(lv_event_t *e)
{
    (void)e;
    s_vol_pct = lv_slider_get_value(s_vol);
    bsp_audio_set_volume((uint32_t)s_vol_pct);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", s_vol_pct);
}

static void delete_overlay(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
}

static void cb_close(lv_event_t *e)
{
    (void)e;
    s_stop_req = true;
    delete_overlay();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *symbol, uint32_t color,
                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 96, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

static void build_ui(void)
{
    if (s_overlay) lv_obj_delete(s_overlay);

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x05080C), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 12, 0);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, s_title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, 396);
    lv_obj_set_pos(title, 6, 10);
    lv_obj_set_style_text_color(title, lv_color_hex(0x39D2C0), 0);

    lv_obj_t *close = lv_button_create(s_overlay);
    lv_obj_set_size(close, 36, 36);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x2A3644), 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, cb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    s_state_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_state_lbl, state_text());
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0x8AB4F8), 0);
    lv_obj_set_pos(s_state_lbl, 6, 46);

    s_elapsed = lv_label_create(s_overlay);
    lv_label_set_text(s_elapsed, "00:00 / 00:00");
    lv_obj_set_style_text_color(s_elapsed, lv_color_hex(0xC9D4E0), 0);
    lv_obj_align(s_elapsed, LV_ALIGN_TOP_RIGHT, -2, 46);

    s_slider = lv_slider_create(s_overlay);
    lv_obj_set_size(s_slider, 456, 10);
    lv_obj_set_pos(s_slider, 6, 78);
    lv_slider_set_range(s_slider, 0, 1000);
    lv_obj_remove_flag(s_slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x2A3644), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x39D2C0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x39D2C0), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_size(s_slider, 8, 8, LV_PART_KNOB);
    lv_obj_set_style_radius(s_slider, 5, LV_PART_MAIN);

    lv_obj_t *vlabel = lv_label_create(s_overlay);
    lv_label_set_text(vlabel, "Volume");
    lv_obj_set_style_text_color(vlabel, lv_color_hex(0x6E7B8B), 0);
    lv_obj_set_pos(vlabel, 6, 110);

    s_vol = lv_slider_create(s_overlay);
    lv_obj_set_size(s_vol, 340, 10);
    lv_obj_set_pos(s_vol, 74, 112);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, s_vol_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_vol, cb_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(0x2A3644), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_vol, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(0x8AB4F8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(0x8AB4F8), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_vol, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_size(s_vol, 14, 14, LV_PART_KNOB);
    lv_obj_set_style_radius(s_vol, 5, LV_PART_MAIN);

    s_vol_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", s_vol_pct);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(0xC9D4E0), 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_TOP_RIGHT, -2, 108);

    lv_obj_t *row = lv_obj_create(s_overlay);
    lv_obj_set_size(row, LV_PCT(100), 60);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 18, 0);

    make_button(row, LV_SYMBOL_PLAY, 0x1B7F4B, cb_play);
    make_button(row, LV_SYMBOL_PAUSE, 0xB08A1E, cb_pause);
    make_button(row, LV_SYMBOL_STOP, 0xB0362E, cb_stop);

    if (!s_timer) s_timer = lv_timer_create(ui_timer_cb, 250, NULL);
}

/* ------------------------------------------------------------------ */
/* Decoder task                                                        */
/* ------------------------------------------------------------------ */

static void play_file(void)
{
    s_pos_frames = 0;
    s_total_frames = 0;
    s_sample_rate = 0;

    FILE *f = fopen(s_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open failed: %s", s_path);
        s_state = ST_ERROR;
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        fclose(f);
        s_state = ST_ERROR;
        return;
    }
    ESP_LOGI(TAG, "playing %s (%ld bytes)", s_title, fsize);

    int bytesLeft = fread(s_inbuf, 1, IN_BUF, f);
    uint8_t *rp = s_inbuf;
    bool started = false;
    bool have_total = false;
    uint32_t cur_rate = 0;
    uint32_t spf = MAX_FRAMES;

    while (!s_stop_req) {
        if (s_state == ST_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (bytesLeft < 2) break;

        int off = MP3FindSyncWord(rp, bytesLeft);
        if (off < 0) break;
        rp += off;
        bytesLeft -= off;

        if (MP3Decode(dec, &rp, &bytesLeft, s_pcm, 0) != ERR_MP3_NONE) {
            if (bytesLeft > 0) { rp++; bytesLeft--; }
            continue;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        int frames = info.outputSamps / (info.nChans ? info.nChans : 1);
        if (info.samprate <= 0 || frames <= 0) continue;
        if (frames > MAX_FRAMES) frames = MAX_FRAMES;

        if (!started) {
            cur_rate = (uint32_t)info.samprate;
            spf = (info.version == 0) ? 1152 : 576;
            if (bsp_audio_pcm_start(cur_rate) != ESP_OK) {
                s_state = ST_ERROR;
                break;
            }
            started = true;
            s_sample_rate = cur_rate;
        } else if ((uint32_t)info.samprate != cur_rate) {
            cur_rate = (uint32_t)info.samprate;
            spf = (info.version == 0) ? 1152 : 576;
            bsp_audio_pcm_start(cur_rate);
            s_sample_rate = cur_rate;
        }

        if (!have_total && fsize > 0 && info.bitrate > 0) {
            int frame_bytes = (144 * info.bitrate) / info.samprate;
            if (frame_bytes < 24) frame_bytes = 24;
            s_total_frames = (uint32_t)(fsize / frame_bytes) * spf;
            have_total = true;
        }

        for (int i = 0; i < frames; i++) {
            if (info.nChans == 2)
                s_mono[i] = (int16_t)(((int)s_pcm[2 * i] + s_pcm[2 * i + 1]) / 2);
            else
                s_mono[i] = s_pcm[i];
        }

        if (bsp_audio_pcm_write(s_mono, frames) != ESP_OK) break;
        s_pos_frames += frames;

        if (bytesLeft < 1024) {
            memmove(s_inbuf, rp, bytesLeft);
            int got = fread(s_inbuf + bytesLeft, 1, IN_BUF - bytesLeft, f);
            bytesLeft += got;
            rp = s_inbuf;
        }
    }

    bsp_audio_pcm_stop();
    MP3FreeDecoder(dec);
    fclose(f);

    if (s_state == ST_ERROR) return;
    if (s_stop_req) {
        s_pos_frames = 0;
        s_state = ST_IDLE;
    } else if (started) {
        s_state = ST_DONE;
    } else {
        s_state = ST_ERROR;
    }
    ESP_LOGI(TAG, "playback ended (state %d)", s_state);
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_evt, EV_START, pdTRUE, pdFALSE, portMAX_DELAY);
        s_stop_req = false;
        s_state = ST_PLAYING;
        play_file();
    }
}

void mp3_player_open(const char *path, const char *title)
{
    if (!s_evt) {
        s_evt = xEventGroupCreate();
        if (!s_evt) return;
    }
    snprintf(s_path, sizeof(s_path), "%s", path);
    snprintf(s_title, sizeof(s_title), "%s", title);

    if (!s_task) {
        if (xTaskCreatePinnedToCore(player_task, "mp3", 6144, NULL, 5, &s_task, 0) != pdPASS) {
            s_task = NULL;
            return;
        }
    }

    build_ui();
    bsp_audio_set_volume((uint32_t)s_vol_pct);
    xEventGroupSetBits(s_evt, EV_START);
}
