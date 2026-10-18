/*
 * MP3 music player for the ESP32-3248S035.
 *
 * Opens an .mp3 file from the SD card, decodes it with the Helix fixed-point
 * decoder and streams the resulting mono PCM into the BSP DAC engine. A modal
 * overlay shows a marquee title, the elapsed / total time, a progress slider
 * and Play / Pause / Stop controls.
 */
#pragma once

#include "lvgl.h"

/**
 * @brief Open the playback overlay for @p path and start decoding.
 * @param path  Full path of the .mp3 file.
 * @param title Display name shown in the scrolling title bar.
 */
void mp3_player_open(const char *path, const char *title);
