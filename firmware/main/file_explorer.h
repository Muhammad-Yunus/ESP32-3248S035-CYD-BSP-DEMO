/* microSD file explorer tab for the ESP32-3248S035. */
#pragma once

#include "lvgl.h"

/* Build the explorer UI inside @p parent (a tabview page). */
void file_explorer_create(lv_obj_t *parent);
