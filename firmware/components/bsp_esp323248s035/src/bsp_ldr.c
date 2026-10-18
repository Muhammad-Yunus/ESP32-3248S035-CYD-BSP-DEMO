/*
 * LDR (light dependent resistor) on ADC1 channel 6 (GPIO34) for the
 * ESP32-3248S035.
 * Port of CDS_ADC from https://github.com/ardnew/ESP32-3248S035
 * The Arduino BSP reads it with 0 dB attenuation (0 - 800 mV full scale).
 */
#include "bsp_esp323248s035.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "bsp_ldr";

static adc_oneshot_unit_handle_t s_adc = NULL;

esp_err_t bsp_ldr_init(void)
{
    if (s_adc) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BSP_LDR_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "new unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BSP_LDR_ADC_CHANNEL, &chan_cfg),
                        TAG, "config channel");
    return ESP_OK;
}

esp_err_t bsp_ldr_read(int *raw)
{
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_ERROR(bsp_ldr_init(), TAG, "init");
    return adc_oneshot_read(s_adc, BSP_LDR_ADC_CHANNEL, raw);
}
