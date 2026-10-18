/*
 * LDR (light dependent resistor) on ADC1 channel 6 (GPIO34) for the
 * ESP32-3248S035.
 *
 * Uses the ADC continuous (DMA) driver to stream samples at up to the SOC
 * maximum sample rate. A dedicated task pinned to CONFIG_BSP_LDR_TASK_CORE
 * blocks on the conversion-done callback, drains the DMA buffer, parses the
 * results and publishes a running average. The sampling therefore happens on
 * a different core than the display/LVGL work, and the task sleeps between
 * conversion frames so it does not starve the idle task.
 */
#include "bsp_esp323248s035.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

static const char *TAG = "bsp_ldr";

#define LDR_FRAME_BYTES     512
#define LDR_STORE_BYTES     4096
#define LDR_PUBLISH_US      5000

static adc_continuous_handle_t s_adc = NULL;
static TaskHandle_t s_task = NULL;
static uint8_t s_frame[LDR_FRAME_BYTES] __attribute__((aligned(4)));
static volatile int s_value = 0;
static volatile uint32_t s_rate = 0;

static bool IRAM_ATTR ldr_on_conv_done(adc_continuous_handle_t handle,
                                       const adc_continuous_evt_data_t *edata, void *user)
{
    (void)handle;
    (void)edata;
    (void)user;
    BaseType_t yield = pdFALSE;
    if (s_task) {
        vTaskNotifyGiveFromISR(s_task, &yield);
    }
    return yield == pdTRUE;
}

static void ldr_task(void *arg)
{
    (void)arg;
    adc_continuous_data_t parsed[LDR_FRAME_BYTES / SOC_ADC_DIGI_RESULT_BYTES];

    uint32_t out_len = 0;
    uint64_t sum = 0;
    uint32_t cnt = 0;
    uint32_t window_samples = 0;
    int64_t window_start = esp_timer_get_time();
    int64_t last_publish = window_start;

    for (;;) {
        /* Sleep until a conversion frame completes (lets the idle task run). */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        while (adc_continuous_read(s_adc, s_frame, sizeof(s_frame), &out_len, 0) == ESP_OK) {
            uint32_t n = 0;
            if (adc_continuous_parse_data(s_adc, s_frame, out_len, parsed, &n) == ESP_OK) {
                for (uint32_t i = 0; i < n; i++) {
                    if (parsed[i].valid) {
                        sum += parsed[i].raw_data;
                        cnt++;
                        window_samples++;
                    }
                }
            }

            int64_t now = esp_timer_get_time();
            if (cnt && now - last_publish >= LDR_PUBLISH_US) {
                s_value = (int)(sum / cnt);
                sum = 0;
                cnt = 0;
                last_publish = now;
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - window_start >= 1000000) {
            s_rate = (uint32_t)((uint64_t)window_samples * 1000000ULL / (uint64_t)(now - window_start));
            window_samples = 0;
            window_start = now;
        }
    }
}

esp_err_t bsp_ldr_init(void)
{
    if (s_adc) return ESP_OK;

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = LDR_STORE_BYTES,
        .conv_frame_size = LDR_FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &s_adc), TAG, "new handle");

    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_0,
        .channel = BSP_LDR_ADC_CHANNEL & 0x7,
        .unit = BSP_LDR_ADC_UNIT,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = CONFIG_BSP_LDR_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc, &cfg), TAG, "config");

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = ldr_on_conv_done,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_register_event_callbacks(s_adc, &cbs, NULL), TAG, "cbs");

    if (xTaskCreatePinnedToCore(ldr_task, "ldr", 8192, NULL, 5, &s_task,
                                CONFIG_BSP_LDR_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(adc_continuous_start(s_adc), TAG, "start");

    ESP_LOGI(TAG, "ADC DMA continuous mode, GPIO%d ch%d, requested %d Hz",
             BSP_LDR_ADC_CHANNEL == ADC_CHANNEL_6 ? 34 : BSP_LDR_ADC_CHANNEL,
             BSP_LDR_ADC_CHANNEL & 0x7, CONFIG_BSP_LDR_SAMPLE_FREQ_HZ);
    return ESP_OK;
}

esp_err_t bsp_ldr_read(int *raw)
{
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_ERROR(bsp_ldr_init(), TAG, "init");
    *raw = s_value;
    return ESP_OK;
}

uint32_t bsp_ldr_sample_rate(void)
{
    return s_rate;
}
