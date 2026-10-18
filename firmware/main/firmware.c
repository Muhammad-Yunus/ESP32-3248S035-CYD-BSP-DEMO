/*
 * LDR monitor + RGB LED control demo for the ESP32-3248S035.
 *
 * The BSP reads the on-board LDR (ADC1 ch6 / GPIO34) one-shot at 1 kHz from a
 * dedicated task pinned to the other core (the DAC sine generator owns I2S0,
 * so the continuous/DMA ADC cannot be used alongside it). This task polls the
 * averaged value every 10 ms, smooths it with an exponential moving average
 * and pushes one point per 100 ms to a FIFO time-series chart (auto-scaled Y
 * axis). Three sliders drive the RGB LED, plus a frequency slider and enable
 * switch for the DAC tone on GPIO26. A second tab hosts a microSD file
 * explorer (see file_explorer.c).
 */
#include "bsp_esp323248s035.h"
#include "file_explorer.h"

#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "firmware";

#define LDR_SAMPLE_MS   10
#define LDR_PUSH_EVERY  10
#define CHART_POINTS    120
#define ADC_MAX         4095

static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_obj_t *s_adc_label;

static int s_obs_min = ADC_MAX;
static int s_obs_max = 0;

static uint8_t s_led[3] = { 0, 0, 0 };

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *value_label;
    uint8_t   channel;
} rgb_ctl_t;

static rgb_ctl_t s_rgb[3];

static lv_obj_t *s_freq_slider;
static lv_obj_t *s_freq_label;
static lv_obj_t *s_vol_slider;
static lv_obj_t *s_vol_label;
static lv_obj_t *s_tone_switch;

#define FREQ_DEFAULT    320
#define TONE_COLOR      lv_color_hex(0xF2B23D)

/* ------------------------------------------------------------------ */
/* RGB LED sliders                                                     */
/* ------------------------------------------------------------------ */

static const char *channel_name(uint8_t channel)
{
    return channel == 0 ? "R" : channel == 1 ? "G" : "B";
}

static void rgb_slider_event_cb(lv_event_t *e)
{
    rgb_ctl_t *ctl = (rgb_ctl_t *)lv_event_get_user_data(e);
    s_led[ctl->channel] = (uint8_t)lv_slider_get_value(ctl->slider);
    lv_label_set_text_fmt(ctl->value_label, "%s  %d",
                          channel_name(ctl->channel), s_led[ctl->channel]);
    bsp_led_set_rgb(s_led[0], s_led[1], s_led[2]);
}

static void create_rgb_row(lv_obj_t *parent, uint8_t channel,
                           const char *name, lv_color_t color, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text_fmt(label, "%s  %d", name, 0);
    lv_obj_set_pos(label, 311, y);
    lv_obj_set_style_text_color(label, color, 0);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, 311, y + 22);
    lv_obj_set_size(slider, 156, 7);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color, LV_PART_KNOB);
    lv_obj_set_style_size(slider, 7, 7, LV_PART_KNOB);
    lv_obj_set_style_pad_left(slider, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(slider, 6, LV_PART_MAIN);

    s_rgb[channel].slider = slider;
    s_rgb[channel].value_label = label;
    s_rgb[channel].channel = channel;
    lv_obj_add_event_cb(slider, rgb_slider_event_cb, LV_EVENT_VALUE_CHANGED, &s_rgb[channel]);
}

/* ------------------------------------------------------------------ */
/* Speaker (DAC) tone controls                                         */
/* ------------------------------------------------------------------ */

static void freq_slider_event_cb(lv_event_t *e)
{
    (void)e;
    uint32_t freq = (uint32_t)lv_slider_get_value(s_freq_slider);
    bsp_audio_set_frequency(freq);
    lv_label_set_text_fmt(s_freq_label, "Freq  %"PRIu32" Hz", bsp_audio_get_frequency());
}

static void volume_slider_event_cb(lv_event_t *e)
{
    (void)e;
    bsp_audio_set_volume((uint32_t)lv_slider_get_value(s_vol_slider));
    lv_label_set_text_fmt(s_vol_label, "Vol  %d%%", (int)bsp_audio_get_volume());
}

static void tone_switch_event_cb(lv_event_t *e)
{
    (void)e;
    bsp_audio_set_enabled(lv_obj_has_state(s_tone_switch, LV_STATE_CHECKED));
}

static void create_freq_row(lv_obj_t *parent, int y)
{
    s_freq_label = lv_label_create(parent);
    lv_label_set_text_fmt(s_freq_label, "Freq  %d Hz", FREQ_DEFAULT);
    lv_obj_set_style_text_color(s_freq_label, TONE_COLOR, 0);
    lv_obj_set_pos(s_freq_label, 311, y);

    s_freq_slider = lv_slider_create(parent);
    lv_obj_set_pos(s_freq_slider, 311, y + 22);
    lv_obj_set_size(s_freq_slider, 156, 7);
    lv_slider_set_range(s_freq_slider, BSP_AUDIO_FREQ_MIN, BSP_AUDIO_FREQ_MAX);
    lv_slider_set_value(s_freq_slider, FREQ_DEFAULT, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_freq_slider, TONE_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_freq_slider, TONE_COLOR, LV_PART_KNOB);
    lv_obj_set_style_size(s_freq_slider, 7, 7, LV_PART_KNOB);
    lv_obj_set_style_pad_left(s_freq_slider, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_freq_slider, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(s_freq_slider, freq_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* Half-size slider (track and thumb), same length as the other rows. */
static void create_volume_row(lv_obj_t *parent, int y)
{
    s_vol_label = lv_label_create(parent);
    lv_label_set_text_fmt(s_vol_label, "Vol  %d%%", (int)BSP_AUDIO_VOLUME_DEFAULT);
    lv_obj_set_style_text_color(s_vol_label, TONE_COLOR, 0);
    lv_obj_set_pos(s_vol_label, 311, y);

    s_vol_slider = lv_slider_create(parent);
    lv_obj_set_pos(s_vol_slider, 311, y + 22);
    lv_obj_set_size(s_vol_slider, 156, 7);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, BSP_AUDIO_VOLUME_DEFAULT, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol_slider, TONE_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, TONE_COLOR, LV_PART_KNOB);
    lv_obj_set_style_size(s_vol_slider, 7, 7, LV_PART_KNOB);
    lv_obj_set_style_pad_left(s_vol_slider, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_vol_slider, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(s_vol_slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void create_tone_row(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *tone_label = lv_label_create(parent);
    lv_label_set_text(tone_label, "Tone");
    lv_obj_set_style_text_color(tone_label, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(tone_label, x, y + 4);

    s_tone_switch = lv_switch_create(parent);
    lv_obj_set_pos(s_tone_switch, x + 48, y);
    lv_obj_set_size(s_tone_switch, 46, 24);
    lv_obj_add_event_cb(s_tone_switch, tone_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ------------------------------------------------------------------ */
/* LDR sampling task                                                   */
/* ------------------------------------------------------------------ */

static void ldr_task(void *arg)
{
    (void)arg;
    int ema = -1;
    int tick = 0;

    for (;;) {
        int v = 0;
        if (bsp_ldr_read(&v) == ESP_OK) {
            ema = (ema < 0) ? v : (ema * 7 + v) / 8;
        }

        if (++tick >= LDR_PUSH_EVERY) {
            tick = 0;
            int val = ema < 0 ? 0 : ema;

            if (bsp_lvgl_lock(pdMS_TO_TICKS(100))) {
                lv_chart_set_next_value(s_chart, s_series, val);
                lv_label_set_text_fmt(s_adc_label, "ADC %4d   %2d%%", val, val * 100 / ADC_MAX);
                if (val < s_obs_min) s_obs_min = val;
                if (val > s_obs_max) s_obs_max = val;
                int lo = s_obs_min - 16;
                if (lo < 0) lo = 0;
                int hi = s_obs_max + 16;
                if (hi < lo + 64) hi = lo + 64;
                lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
                bsp_lvgl_unlock();
            }

            static uint32_t nlog = 0;
            if (++nlog % 10 == 0) {
                ESP_LOGI(TAG, "LDR ema=%d (min=%d max=%d)  %"PRIu32" Hz",
                         val, s_obs_min, s_obs_max, bsp_ldr_sample_rate());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LDR_SAMPLE_MS));
    }
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *create_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111822), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1F2A37), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_margin_all(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    return panel;
}

static void build_monitoring_tab(lv_obj_t *parent)
{
    /* 6 px margin / gap between the three containers, 8 px padding */
    create_panel(parent, 6, 6, 290, 268);
    create_panel(parent, 302, 6, 174, 132);
    create_panel(parent, 302, 144, 174, 130);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "LDR Monitor");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(title, 14, 14);

    s_adc_label = lv_label_create(parent);
    lv_label_set_text(s_adc_label, "ADC ----   --%");
    lv_obj_set_style_text_color(s_adc_label, lv_color_hex(0x39D2C0), 0);
    lv_obj_set_pos(s_adc_label, 170, 14);

    s_chart = lv_chart_create(parent);
    lv_obj_set_pos(s_chart, 14, 42);
    lv_obj_set_size(s_chart, 274, 196);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 64);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 5, 8);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(0x1F2A37), 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x1F2A37), LV_PART_MAIN);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(0x39D2C0), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);

    lv_obj_t *caption = lv_label_create(parent);
    lv_label_set_text(caption, "time ->   (100 ms / sample)");
    lv_obj_set_style_text_color(caption, lv_color_hex(0x6E7B8B), 0);
    lv_obj_set_pos(caption, 14, 246);

    lv_obj_t *led_title = lv_label_create(parent);
    lv_label_set_text(led_title, "LED control");
    lv_obj_set_style_text_color(led_title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(led_title, 311, 15);

    create_rgb_row(parent, 0, "R", lv_color_hex(0xF04444), 34);
    create_rgb_row(parent, 1, "G", lv_color_hex(0x44E05A), 66);
    create_rgb_row(parent, 2, "B", lv_color_hex(0x4488FF), 98);

    lv_obj_t *spk_title = lv_label_create(parent);
    lv_label_set_text(spk_title, "Speaker (DAC)");
    lv_obj_set_style_text_color(spk_title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(spk_title, 311, 152);

    create_freq_row(parent, 172);
    create_volume_row(parent, 210);
    create_tone_row(parent, 311, 248);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F14), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tv = lv_tabview_create(scr);
    lv_obj_set_size(tv, 480, 320);
    lv_obj_set_pos(tv, 0, 0);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x0B0F14), 0);
    lv_tabview_set_tab_bar_size(tv, 38);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_pad_bottom(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    /* thin baseline under the whole tab bar */
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x1F2A37), 0);

    lv_obj_t *mon = lv_tabview_add_tab(tv, "Monitoring & Control");
    lv_obj_t *files = lv_tabview_add_tab(tv, "File Explorer");

    for (uint32_t i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_tabview_get_tab_button(tv, i);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xC9D4E0), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(0x6E7B8B), LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1B2A3A), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x111822), LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        /* the active tab gets a thick accent underline instead */
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x39D2C0), LV_STATE_CHECKED);
    }

    lv_obj_set_style_bg_color(mon, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_pad_all(mon, 0, 0);
    lv_obj_remove_flag(mon, LV_OBJ_FLAG_SCROLLABLE);

    build_monitoring_tab(mon);
    file_explorer_create(files);
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_init());

    if (bsp_sd_mount("/sdcard") != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted, file explorer will be empty");
    }

    if (!bsp_lvgl_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return;
    }
    build_ui();
    bsp_lvgl_unlock();

    bsp_audio_set_frequency(FREQ_DEFAULT);
    bsp_audio_set_volume(BSP_AUDIO_VOLUME_DEFAULT);
    bsp_audio_set_enabled(false);

    xTaskCreate(ldr_task, "ldr", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "demo started (LDR chart + RGB LED + DAC speaker + file explorer)");
}
