#include <stdbool.h>
#include <stdio.h>

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

static void log_nfc_event(uint8_t status)
{
    ESP_LOGI(TAG, "NFC事件状态：0x%02X%s%s%s%s%s%s%s%s", status,
             (status & NFC_EVENT_RF_USER) ? " RF_USER" : "",
             (status & NFC_EVENT_RF_ACTIVITY) ? " RF_ACTIVITY" : "",
             (status & NFC_EVENT_RF_INTERRUPT) ? " RF_INTERRUPT" : "",
             (status & NFC_EVENT_FIELD_FALLING) ? " FIELD_FALLING" : "",
             (status & NFC_EVENT_FIELD_RISING) ? " FIELD_RISING" : "",
             (status & NFC_EVENT_RF_PUT_MSG) ? " RF_PUT_MSG" : "",
             (status & NFC_EVENT_RF_GET_MSG) ? " RF_GET_MSG" : "",
             (status & NFC_EVENT_RF_WRITE) ? " RF_WRITE" : "");
}

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(display_show_test_pattern());
    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(buzzer_init());

    bool nfc_ready = false;
    esp_err_t nfc_init_ret = nfc_init();
    if (nfc_init_ret != ESP_OK) {
        ESP_LOGW(TAG, "NFC初始化失败：%s，按键测试继续运行", esp_err_to_name(nfc_init_ret));
        display_fill_color(COLOR_MAGENTA);
        vTaskDelay(pdMS_TO_TICKS(800));
    } else {
        esp_err_t nfc_test_ret = nfc_self_test();
        ESP_LOGI(TAG, "ST25DV04KC非破坏性自检：%s", esp_err_to_name(nfc_test_ret));

        esp_err_t ndef_ret = ESP_FAIL;
        if (nfc_test_ret == ESP_OK) {
            /* 仅在标签没有有效NDEF时写入默认URI，避免每次开机磨损和覆盖原数据。 */
            ndef_ret = nfc_ensure_ndef_uri("example.com/card");
            ESP_LOGI(TAG, "NDEF检查：%s", esp_err_to_name(ndef_ret));
        }

        nfc_ready = nfc_test_ret == ESP_OK;
        display_fill_color(nfc_test_ret == ESP_OK && ndef_ret == ESP_OK ? COLOR_CYAN
                                                                        : COLOR_MAGENTA);
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

        if (nfc_ready) {
            uint8_t nfc_status = 0;
            esp_err_t event_ret = nfc_get_interrupt_status(&nfc_status);
            if (event_ret == ESP_OK) {
                log_nfc_event(nfc_status);
            } else if (event_ret != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "读取NFC事件失败：%s", esp_err_to_name(event_ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
