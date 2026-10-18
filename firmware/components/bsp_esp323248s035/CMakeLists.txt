idf_component_register(
    SRCS
        "src/bsp_tft.c"
        "src/bsp_touch.c"
        "src/bsp_led.c"
        "src/bsp_buzzer.c"
        "src/bsp_ldr.c"
        "src/bsp_sd.c"
        "src/bsp.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2c
        esp_driver_sdspi
        sdmmc
        esp_adc
        esp_timer
        fatfs
        lvgl
)

# The LVGL glue code is only compiled when the port is enabled.
if(CONFIG_BSP_LVGL)
    target_sources(${COMPONENT_LIB} PRIVATE "src/bsp_lvgl.c")
endif()
