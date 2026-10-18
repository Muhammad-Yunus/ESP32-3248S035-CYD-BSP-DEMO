#include "bsp_esp323248s035.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "firmware";

/* Standard RGB565 values. Colour data is byte-swapped before it is handed to
 * esp_lcd because the ST7796 is a big-endian receiver while the ESP32 stores
 * RGB565 little-endian (same as pushColors(..., true) in the references). */
static const uint16_t BAR_COLOURS[] = {
    0xF800, /* red     */
    0x07E0, /* green   */
    0x001F, /* blue    */
    0xFFFF, /* white   */
    0x0000, /* black   */
    0xFFE0, /* yellow  */
    0x07FF, /* cyan    */
    0xF81F, /* magenta */
};
#define N_BARS (sizeof(BAR_COLOURS) / sizeof(BAR_COLOURS[0]))

static uint16_t s_bar[BSP_TFT_H_RES * (BSP_TFT_V_RES / N_BARS + 1)];

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_tft_init());
    ESP_LOGI(TAG, "TFT ready (%ux%u, rotation %d)",
             bsp_tft_width(), bsp_tft_height(), bsp_tft_get_rotation());

    esp_lcd_panel_handle_t panel = bsp_tft_get_panel();
    uint16_t w = bsp_tft_width();
    uint16_t h = bsp_tft_height();
    uint16_t bar_h = h / N_BARS;

    for (int b = 0; b < (int)N_BARS; b++) {
        uint16_t wire = (uint16_t)((BAR_COLOURS[b] >> 8) | (BAR_COLOURS[b] << 8));
        for (uint32_t i = 0; i < (uint32_t)w * bar_h; i++) {
            s_bar[i] = wire;
        }
        int y = b * bar_h;
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, w, y + bar_h, s_bar));
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "colour bars drawn (MADCTL BGR bit + byte-swapped RGB565)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
