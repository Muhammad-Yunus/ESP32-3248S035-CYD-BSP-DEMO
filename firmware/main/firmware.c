/*
 * LDR monitor + RGB LED control demo for the ESP32-3248S035.
 *
 * The BSP streams the on-board LDR (ADC1 ch6 / GPIO34) through the ADC
 * continuous (DMA) driver at up to 2 MHz on a dedicated task pinned to the
 * other core. This task polls the averaged value every 10 ms, smooths it with
 * an exponential moving average and pushes one point per 100 ms to a FIFO
 * time-series chart (auto-scaled Y axis). Three sliders drive the RGB LED.
 */
#include "bsp_esp323248s035.h"

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
    lv_obj_set_pos(label, 312, y);
    lv_obj_set_style_text_color(label, color, 0);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, 312, y + 22);
    lv_obj_set_size(slider, 158, 14);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color, LV_PART_KNOB);

    s_rgb[channel].slider = slider;
    s_rgb[channel].value_label = label;
    s_rgb[channel].channel = channel;
    lv_obj_add_event_cb(slider, rgb_slider_event_cb, LV_EVENT_VALUE_CHANGED, &s_rgb[channel]);
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
                ESP_LOGI(TAG, "LDR ema=%d (min=%d max=%d)  DMA %"PRIu32" Hz",
                         val, s_obs_min, s_obs_max, bsp_ldr_sample_rate());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LDR_SAMPLE_MS));
    }
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F14), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LDR Monitor");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(title, 12, 8);

    s_adc_label = lv_label_create(scr);
    lv_label_set_text(s_adc_label, "ADC ----   --%");
    lv_obj_set_style_text_color(s_adc_label, lv_color_hex(0x39D2C0), 0);
    lv_obj_set_pos(s_adc_label, 170, 8);

    s_chart = lv_chart_create(scr);
    lv_obj_set_pos(s_chart, 10, 40);
    lv_obj_set_size(s_chart, 292, 200);
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

    lv_obj_t *caption = lv_label_create(scr);
    lv_label_set_text(caption, "time ->   (100 ms / sample)");
    lv_obj_set_style_text_color(caption, lv_color_hex(0x6E7B8B), 0);
    lv_obj_set_pos(caption, 12, 246);

    lv_obj_t *led_title = lv_label_create(scr);
    lv_label_set_text(led_title, "LED control");
    lv_obj_set_style_text_color(led_title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(led_title, 312, 8);

    create_rgb_row(scr, 0, "R", lv_color_hex(0xF04444), 42);
    create_rgb_row(scr, 1, "G", lv_color_hex(0x44E05A), 130);
    create_rgb_row(scr, 2, "B", lv_color_hex(0x4488FF), 218);
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_init());

    if (!bsp_lvgl_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return;
    }
    build_ui();
    bsp_lvgl_unlock();

    xTaskCreate(ldr_task, "ldr", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "demo started (LDR chart + RGB LED sliders)");
}
