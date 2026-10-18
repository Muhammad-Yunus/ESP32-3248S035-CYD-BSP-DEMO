/*
 * Sine tone generator on GPIO26 (DAC channel 1) driving the on-board FM8002A
 * class-AB audio amplifier, for the ESP32-3248S035.
 *
 * The DAC runs in continuous (DMA) mode. A dedicated task keeps a
 * phase-continuous sine (looked up from a 1024-entry table) streamed into the
 * DMA descriptors, so the CPU only ever generates a few tens of milliseconds
 * of samples ahead of playback and spends the rest of the time blocked. The
 * output frequency can be changed from any task through
 * bsp_audio_set_frequency().
 */
#include "bsp_esp323248s035.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/dac_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "bsp_audio";

#define AUDIO_DESC_NUM      8
#define AUDIO_DMA_BUF_SIZE  512
#define AUDIO_BUF_SAMPLES   (CONFIG_BSP_AUDIO_BUF_SAMPLES)
#define AUDIO_LUT_SIZE      1024
#define AUDIO_TASK_PRIO     6

static dac_continuous_handle_t s_dac = NULL;
static TaskHandle_t s_task = NULL;
static int16_t s_lut[AUDIO_LUT_SIZE + 1];
static uint8_t s_buf[AUDIO_BUF_SAMPLES];

static volatile uint32_t s_freq_hz = CONFIG_BSP_AUDIO_DEFAULT_FREQ_HZ;
static volatile uint32_t s_volume_pct = BSP_AUDIO_VOLUME_DEFAULT;
static volatile bool s_enabled = false;

/* Phase increment (Q32) that advances the LUT index by (freq/rate) cycles. */
static uint32_t freq_to_inc(uint32_t freq_hz)
{
    return (uint32_t)(((uint64_t)freq_hz << 32) / CONFIG_BSP_AUDIO_SAMPLE_RATE_HZ);
}

static void audio_task(void *arg)
{
    (void)arg;
    uint32_t phase = 0;
    uint32_t cur_freq = 0;
    uint32_t inc = freq_to_inc(s_freq_hz);

    for (;;) {
        uint32_t freq = s_freq_hz;
        if (freq != cur_freq) {
            cur_freq = freq;
            inc = freq_to_inc(freq);
        }

        if (s_enabled) {
            int32_t vol = (int32_t)s_volume_pct;
            for (size_t i = 0; i < AUDIO_BUF_SAMPLES; i++) {
                uint32_t idx = phase >> 22;             /* 10-bit table index */
                uint32_t frac = (phase >> 14) & 0xFF;   /* 8-bit fraction      */
                int32_t v = s_lut[idx] +
                            (((int32_t)(s_lut[idx + 1] - s_lut[idx]) * (int32_t)frac) >> 8);
                int32_t s = 128 + (v * vol) / 3000;    /* gain trimmed down */
                if (s < 0) s = 0;
                else if (s > 255) s = 255;
                s_buf[i] = (uint8_t)s;
                phase += inc;
            }
        } else {
            memset(s_buf, 128, AUDIO_BUF_SAMPLES);      /* mid-rail = silence */
        }

        if (dac_continuous_write(s_dac, s_buf, AUDIO_BUF_SAMPLES, NULL, -1) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t bsp_audio_init(void)
{
    if (s_dac) return ESP_OK;

    for (int i = 0; i <= AUDIO_LUT_SIZE; i++) {
        double ph = 2.0 * M_PI * (double)i / (double)AUDIO_LUT_SIZE;
        s_lut[i] = (int16_t)lround((double)CONFIG_BSP_AUDIO_AMPLITUDE * sin(ph));
    }
    s_lut[AUDIO_LUT_SIZE] = s_lut[0];

    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH1,      /* GPIO26 */
        .desc_num = AUDIO_DESC_NUM,
        .buf_size = AUDIO_DMA_BUF_SIZE,
        .freq_hz = CONFIG_BSP_AUDIO_SAMPLE_RATE_HZ,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    ESP_RETURN_ON_ERROR(dac_continuous_new_channels(&cfg, &s_dac), TAG, "new channels");
    ESP_RETURN_ON_ERROR(dac_continuous_enable(s_dac), TAG, "enable");

    if (xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, AUDIO_TASK_PRIO, &s_task,
                                CONFIG_BSP_AUDIO_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DAC continuous on GPIO26, %d Hz sample rate, default %d Hz, vol %d%%",
             CONFIG_BSP_AUDIO_SAMPLE_RATE_HZ, CONFIG_BSP_AUDIO_DEFAULT_FREQ_HZ,
             (int)BSP_AUDIO_VOLUME_DEFAULT);
    return ESP_OK;
}

esp_err_t bsp_audio_set_frequency(uint32_t freq_hz)
{
    if (freq_hz < BSP_AUDIO_FREQ_MIN) freq_hz = BSP_AUDIO_FREQ_MIN;
    if (freq_hz > BSP_AUDIO_FREQ_MAX) freq_hz = BSP_AUDIO_FREQ_MAX;
    s_freq_hz = freq_hz;
    return ESP_OK;
}

uint32_t bsp_audio_get_frequency(void)
{
    return s_freq_hz;
}

esp_err_t bsp_audio_set_enabled(bool enabled)
{
    s_enabled = enabled;
    return ESP_OK;
}

bool bsp_audio_is_enabled(void)
{
    return s_enabled;
}

esp_err_t bsp_audio_set_volume(uint32_t percent)
{
    if (percent > 100) percent = 100;
    s_volume_pct = percent;
    return ESP_OK;
}

uint32_t bsp_audio_get_volume(void)
{
    return s_volume_pct;
}
