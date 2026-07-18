#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 320

esp_err_t display_init(void);
esp_err_t display_fill_color(uint16_t color);
esp_err_t display_show_test_pattern(void);

#ifdef __cplusplus
}
#endif

#endif
