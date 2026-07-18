#include <stdio.h>
#include <stdbool.h>

#include "button.h"
#include "buzzer.h"
#include "display.h"
#include "nfc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

static const char *TAG = "main";

static uint16_t button_color(button_id_t id)
{
    switch (id) {
    case BUTTON_SW2:
        return COLOR_WHITE;
    case BUTTON_SW3:
        return COLOR_RED;
    case BUTTON_SW4:
        return COLOR_GREEN;
    case BUTTON_SW5:
        return COLOR_BLUE;
    default:
        return COLOR_BLACK;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(display_show_test_pattern());
    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(buzzer_init());

    bool nfc_ready = nfc_init() == ESP_OK;
    if (!nfc_ready) {
        ESP_LOGW(TAG, "NFC init failed, button test keeps running");
        display_fill_color(COLOR_MAGENTA);
    } else {
        esp_err_t nfc_test_ret = nfc_self_test();
        ESP_LOGI(TAG, "ST25DV04KC self test: %s", esp_err_to_name(nfc_test_ret));
        display_fill_color(nfc_test_ret == ESP_OK ? COLOR_CYAN : COLOR_MAGENTA);
        vTaskDelay(pdMS_TO_TICKS(800));
    }

    display_fill_color(COLOR_BLACK);

    while (1) {
        button_event_t event = {0};
        if (button_get_event(&event)) {
            ESP_LOGI(TAG, "%s %s", button_name(event.id), event.pressed ? "pressed" : "released");
            if (event.pressed) {
                display_fill_color(button_color(event.id));
                buzzer_beep(2000 + event.id * 400, 80);
            } else {
                display_fill_color(COLOR_BLACK);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
