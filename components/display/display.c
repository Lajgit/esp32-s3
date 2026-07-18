#include "display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DISPLAY_SPI_HOST SPI2_HOST

#define DISPLAY_PIN_CS    GPIO_NUM_8
#define DISPLAY_PIN_SCLK  GPIO_NUM_4
#define DISPLAY_PIN_MOSI  GPIO_NUM_5
#define DISPLAY_PIN_DC    GPIO_NUM_6
#define DISPLAY_PIN_RST   GPIO_NUM_7
#define DISPLAY_PIN_LEDK  GPIO_NUM_15

#define DISPLAY_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_LINE_COUNT     20
#define DISPLAY_BRIGHTNESS_MAX 255

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;

static esp_err_t display_backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "config backlight timer failed");

    ledc_channel_config_t channel = {
        .gpio_num = DISPLAY_PIN_LEDK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = DISPLAY_BRIGHTNESS_MAX,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "config backlight channel failed");

    return ESP_OK;
}

esp_err_t display_init(void)
{
    if (s_panel) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_backlight_init(), TAG, "init backlight failed");

    spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_LINE_COUNT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "init spi bus failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISPLAY_PIN_DC,
        .cs_gpio_num = DISPLAY_PIN_CS,
        .pclk_hz = DISPLAY_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST,
                                                 &io_config, &io_handle),
                        TAG, "create lcd io failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel),
                        TAG, "create st7789 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "mirror panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert panel color failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "turn display on failed");

    return ESP_OK;
}

esp_err_t display_fill_color(uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "display is not initialized");

    const size_t pixel_count = DISPLAY_WIDTH * DISPLAY_LINE_COUNT;
    uint16_t *line = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(line, ESP_ERR_NO_MEM, TAG, "alloc line buffer failed");

    for (size_t i = 0; i < pixel_count; i++) {
        line[i] = color;
    }

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < DISPLAY_HEIGHT; y += DISPLAY_LINE_COUNT) {
        int y_end = y + DISPLAY_LINE_COUNT;
        if (y_end > DISPLAY_HEIGHT) {
            y_end = DISPLAY_HEIGHT;
        }

        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISPLAY_WIDTH, y_end, line);
        if (ret != ESP_OK) {
            break;
        }
    }

    heap_caps_free(line);
    return ret;
}

esp_err_t display_show_test_pattern(void)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "display is not initialized");

    static const uint16_t colors[] = {
        0xFFFF,
        0x0000,
        0xF800,
        0x07E0,
        0x001F,
    };

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        ESP_RETURN_ON_ERROR(display_fill_color(colors[i]), TAG, "fill display failed");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    const size_t pixel_count = DISPLAY_WIDTH * DISPLAY_LINE_COUNT;
    uint16_t *line = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(line, ESP_ERR_NO_MEM, TAG, "alloc test pattern buffer failed");

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < DISPLAY_HEIGHT; y += DISPLAY_LINE_COUNT) {
        int y_end = y + DISPLAY_LINE_COUNT;
        if (y_end > DISPLAY_HEIGHT) {
            y_end = DISPLAY_HEIGHT;
        }

        for (int row = y; row < y_end; row++) {
            for (int x = 0; x < DISPLAY_WIDTH; x++) {
                int band = x / 30;
                uint16_t color = 0x0000;
                if (band == 0) {
                    color = 0xFFFF;
                } else if (band == 1) {
                    color = 0x0000;
                } else if (band == 2) {
                    color = 0x001F;
                } else if (band == 3) {
                    color = 0xF81F;
                } else if (band == 4) {
                    color = 0xFFE0;
                } else if (band == 5) {
                    color = 0x07FF;
                } else if (band == 6) {
                    color = 0xF800;
                } else {
                    color = 0x07E0;
                }
                line[(row - y) * DISPLAY_WIDTH + x] = color;
            }
        }

        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISPLAY_WIDTH, y_end, line);
        if (ret != ESP_OK) {
            break;
        }
    }

    heap_caps_free(line);
    return ret;
}
